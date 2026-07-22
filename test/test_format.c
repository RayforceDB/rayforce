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
#include "lang/env.h"
#include "lang/eval.h"
#include <string.h>
#include <limits.h>
#include <math.h>

/* Forward-declare runtime API */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t *__RUNTIME;

/* ---- Setup / Teardown ---- */

static void fmt_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void fmt_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* Setup that also initialises env/builtins (needed for fn objects) */
static void fmt_setup_full(void) {
    ray_runtime_create(0, NULL);
}

static void fmt_teardown_full(void) {
    ray_runtime_destroy(__RUNTIME);
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

/* ---- Test: fmt_sym fallback (invalid sym id -> "'") ---- */
static test_result_t test_fmt_sym_invalid(void) {
    /* id -1 is out of range, ray_sym_str returns NULL -> empty symbol "'" */
    ray_t* obj = ray_sym(-1);
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_FALSE(RAY_IS_ERR(obj));
    ray_t* result = ray_fmt(obj, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "'"));
    ray_release(result);
    ray_release(obj);
    PASS();
}

/* ---- Test: null_literal default case (non-standard type) ---- */
static test_result_t test_fmt_null_default(void) {
    /* Force an atom with null bit set and a type that null_literal doesn't know.
     * We craft it via ray_typed_null(0) which corresponds to -(RAY_LIST=0)=0
     * but since 0 is RAY_LIST which has no -RAY_LIST case in null_literal,
     * it will fall to the default "null" branch. */
    ray_t* obj = ray_typed_null(0);  /* type 0 = RAY_LIST, no atom null form */
    if (!obj || RAY_IS_ERR(obj)) PASS(); /* skip if not supported */
    ray_t* result = ray_fmt(obj, 1);
    TEST_ASSERT_NOT_NULL(result);
    /* Either "null" from null_literal default or raw value -- just no crash */
    ray_release(result);
    ray_release(obj);
    PASS();
}

/* ---- Test: format a lambda via eval ---- */
static test_result_t test_fmt_lambda(void) {
    /* Eval returns the lambda object */
    ray_t* fn = ray_eval_str("(fn [x] (* x 2))");
    TEST_ASSERT_NOT_NULL(fn);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fn));
    TEST_ASSERT_EQ_I(fn->type, RAY_LAMBDA);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "lambda"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

/* ---- Test: fmt_raw_elem with a LIST-typed table column ---- */
static test_result_t test_fmt_table_list_col(void) {
    /* Build a list, then put it as a "column" in a table.
     * ray_table_add_col accepts any vec — if the col is a list,
     * fmt_raw_elem will hit the RAY_LIST case. */
    ray_t* items = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(items);
    items = ray_list_append(items, ray_i64(1));
    items = ray_list_append(items, ray_i64(2));
    items = ray_list_append(items, ray_i64(3));
    TEST_ASSERT_FALSE(RAY_IS_ERR(items));

    int64_t id_c = ray_sym_intern("col", 3);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_c, items);
    if (RAY_IS_ERR(tbl)) {
        /* If table rejects list columns, just skip */
        ray_release(tbl);
        PASS();
    }
    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    /* just verify no crash */
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: table mode 2 (no row/col limits → heap alloc path) ---- */
static test_result_t test_fmt_table_mode2(void) {
    /* With FMT_TABLE_MAX_HEIGHT+5 rows and FMT_TABLE_MAX_WIDTH+2 cols,
     * mode 2 does NOT clamp, so table_width and table_height stay large
     * and heap_alloc becomes true. */
    int64_t ncols = FMT_TABLE_MAX_WIDTH + 2;
    int64_t nrows = FMT_TABLE_MAX_HEIGHT + 5;
    ray_t* tbl = ray_table_new((int32_t)nrows);
    TEST_ASSERT_NOT_NULL(tbl);
    for (int64_t ci = 0; ci < ncols && !RAY_IS_ERR(tbl); ci++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "c%d", (int)ci);
        int64_t id = ray_sym_intern(nm, strlen(nm));
        ray_t* col = ray_vec_new(RAY_I64, nrows);
        for (int64_t ri = 0; ri < nrows; ri++) {
            int64_t v = ci * 100 + ri;
            col = ray_vec_append(col, &v);
            if (RAY_IS_ERR(col)) break;
        }
        if (RAY_IS_ERR(col)) { ray_release(col); break; }
        tbl = ray_table_add_col(tbl, id, col);
    }
    if (!RAY_IS_ERR(tbl)) {
        ray_t* result = ray_fmt(tbl, 2);
        TEST_ASSERT_NOT_NULL(result);
        /* Just verify no crash and contains some data */
        const char* s = ray_str_ptr(result);
        TEST_ASSERT_NOT_NULL(s);
        ray_release(result);
    }
    ray_release(tbl);
    PASS();
}

/* ---- Test: table with a short column (triggers "NA" cells) ---- */
static test_result_t test_fmt_table_short_col(void) {
    /* Table with 5 rows but one column has only 2 elements */
    int64_t nrows = 5;
    ray_t* tbl = ray_table_new((int32_t)nrows);
    TEST_ASSERT_NOT_NULL(tbl);

    int64_t id_a = ray_sym_intern("full", 4);
    int64_t id_b = ray_sym_intern("short", 5);

    /* full col: 5 elements */
    int64_t full_raw[] = {1, 2, 3, 4, 5};
    ray_t* col_full = ray_vec_from_raw(RAY_I64, full_raw, 5);

    /* short col: only 2 elements */
    int64_t short_raw[] = {10, 20};
    ray_t* col_short = ray_vec_from_raw(RAY_I64, short_raw, 2);

    tbl = ray_table_add_col(tbl, id_a, col_full);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_short);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* Short column should show NA for missing rows */
    TEST_ASSERT_NOT_NULL(strstr(s, "NA"));
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: null_literal default (RAY_TABLE null atom -> "null") ---- */
static test_result_t test_fmt_null_table_atom(void) {
    /* Passing -RAY_TABLE (=-98) as the type to ray_typed_null creates an atom
     * with type=-98.  null_literal(-(-98)) = null_literal(98) = RAY_TABLE which
     * has no case -> "null" default. */
    ray_t* obj = ray_typed_null(-RAY_TABLE);
    if (!obj || RAY_IS_ERR(obj)) PASS();
    ray_t* result = ray_fmt(obj, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "null"));
    ray_release(result);
    ray_release(obj);
    PASS();
}

/* ---- Test: table with both hidden rows AND hidden cols (lines 926-927) ---- */
static test_result_t test_fmt_table_wide_and_tall(void) {
    /* Need > MAX_WIDTH cols AND > MAX_HEIGHT rows in mode 1 */
    int64_t ncols = FMT_TABLE_MAX_WIDTH + 2;
    int64_t nrows = FMT_TABLE_MAX_HEIGHT + 5;
    ray_t* tbl = ray_table_new((int32_t)nrows);
    TEST_ASSERT_NOT_NULL(tbl);
    for (int64_t ci = 0; ci < ncols && !RAY_IS_ERR(tbl); ci++) {
        char nm[8];
        snprintf(nm, sizeof(nm), "c%d", (int)ci);
        int64_t id = ray_sym_intern(nm, strlen(nm));
        ray_t* col = ray_vec_new(RAY_I64, nrows);
        for (int64_t ri = 0; ri < nrows; ri++) {
            int64_t v = ci * 100 + ri;
            col = ray_vec_append(col, &v);
            if (RAY_IS_ERR(col)) break;
        }
        if (RAY_IS_ERR(col)) { ray_release(col); break; }
        tbl = ray_table_add_col(tbl, id, col);
    }
    if (!RAY_IS_ERR(tbl)) {
        /* mode 1 clamps both, has_hidden_cols=true AND has_hidden_rows=true */
        ray_t* result = ray_fmt(tbl, 1);
        TEST_ASSERT_NOT_NULL(result);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        const char* s = ray_str_ptr(result);
        TEST_ASSERT_NOT_NULL(s);
        ray_release(result);
    }
    ray_release(tbl);
    PASS();
}

/* ---- Test: fmt_raw_elem null element in RAY_LIST vector ---- */
static test_result_t test_fmt_raw_elem_list_null(void) {
    /* A RAY_LIST-typed "vector" where an element (child) is NULL.
     * We build a table with a list column to exercise fmt_raw_elem's RAY_LIST case.
     * When the list has a NULL element, it hits the "null" path at line 404. */
    ray_t* items = ray_list_new(2);
    TEST_ASSERT_NOT_NULL(items);
    items = ray_list_append(items, ray_i64(1));
    items = ray_list_append(items, NULL);  /* NULL child */
    /* items->len should be 2, items[1] = NULL */

    int64_t id_c = ray_sym_intern("lc", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_c, items);
    if (RAY_IS_ERR(tbl)) {
        ray_release(tbl);
        PASS(); /* skip if table rejects list cols */
    }
    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: table with short col that triggers NA in head half ---- */
static test_result_t test_fmt_table_na_head(void) {
    /* With nrows=5 (half=2), a col of len=1 has ri=0 hit, ri=1 miss -> NA in head */
    int64_t nrows = 5;
    int64_t id_a = ray_sym_intern("fa", 2);
    int64_t id_b = ray_sym_intern("sb", 2);

    int64_t full_raw[] = {1, 2, 3, 4, 5};
    ray_t* col_full = ray_vec_from_raw(RAY_I64, full_raw, 5);

    /* col_short has 1 element only (less than half=2) */
    int64_t s_raw[] = {99};
    ray_t* col_short = ray_vec_from_raw(RAY_I64, s_raw, 1);

    ray_t* tbl = ray_table_new((int32_t)nrows);
    tbl = ray_table_add_col(tbl, id_a, col_full);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_short);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "NA"));
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: ray_fmt_set_precision and ray_fmt_set_width ---- */
static test_result_t test_fmt_set_precision(void) {
    /* set valid precision */
    ray_fmt_set_precision(4);
    ray_t* result = ray_fmt(ray_f64(3.14159), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* with precision 4 we should have more digits than the default 2 */
    TEST_ASSERT_NOT_NULL(strstr(s, "3.14"));
    ray_release(result);
    /* restore default */
    ray_fmt_set_precision(2);
    PASS();
}

static test_result_t test_fmt_set_width(void) {
    /* set a valid width */
    ray_fmt_set_width(40);
    int64_t raw[] = {1, 2, 3, 4, 5};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    TEST_ASSERT_NOT_NULL(vec);
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_release(vec);
    /* restore default */
    ray_fmt_set_width(80);
    PASS();
}

/* ---- Test: ray_type_name for F32, INDEX, unknown ---- */
static test_result_t test_type_name_f32(void) {
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_F32),  "F32");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_F32), "f32");
    PASS();
}

static test_result_t test_type_name_index(void) {
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_INDEX), "INDEX");
    PASS();
}

static test_result_t test_type_name_unknown(void) {
    /* type 127 is not a known type — should return "?" */
    const char* n = ray_type_name(127);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_STR_EQ(n, "?");
    PASS();
}

/* ---- Test: format atom types (u8, i16, i32, f32, date, time, timestamp) ---- */
static test_result_t test_fmt_atom_u8(void) {
    ray_t* result = ray_fmt(ray_u8(0xAB), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0xab"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_i16(void) {
    ray_t* result = ray_fmt(ray_i16(1234), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "1234"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_i32(void) {
    ray_t* result = ray_fmt(ray_i32(99999), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "99999"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_f32(void) {
    ray_t* result = ray_fmt(ray_f32(2.5f), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "2.5"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_date(void) {
    /* day 0 from epoch (2000-01-01 in rayforce) */
    ray_t* result = ray_fmt(ray_date(0), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_time(void) {
    /* 1 hour 2 min 3 sec 456 ms = 3723456 ms */
    ray_t* result = ray_fmt(ray_time(3723456), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "01:02:03.456"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_time_neg(void) {
    /* negative time should start with '-' */
    ray_t* result = ray_fmt(ray_time(-1000), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strchr(s, '-'));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_timestamp(void) {
    /* nanoseconds for 2000-01-01 00:00:00.000000000 = 0 */
    ray_t* result = ray_fmt(ray_timestamp(0), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* Should contain the D separator */
    TEST_ASSERT_NOT_NULL(strchr(s, 'D'));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_sym(void) {
    int64_t id = ray_sym_intern("foo", 3);
    ray_t* result = ray_fmt(ray_sym(id), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "foo"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_atom_str(void) {
    ray_t* str = ray_str("hello", 5);
    ray_t* result = ray_fmt(str, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "\"hello\""));
    ray_release(result);
    ray_release(str);
    PASS();
}

static test_result_t test_fmt_atom_guid(void) {
    uint8_t bytes[16] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,
                         0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10};
    ray_t* g = ray_guid(bytes);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(g));
    ray_t* result = ray_fmt(g, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* GUID format: 8-4-4-4-12 hex chars separated by '-' */
    TEST_ASSERT_NOT_NULL(strchr(s, '-'));
    ray_release(result);
    ray_release(g);
    PASS();
}

/* ---- Test: null_literal coverage (bool, u8, f32, date, str, guid) ---- */
static test_result_t test_fmt_null_bool(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_BOOL), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Nb"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_null_u8(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_U8), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Nu"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_null_f32(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_F32), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Ne"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_null_date(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_DATE), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Nd"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_null_str(void) {
    /* A STR atom has no distinct null: ray_typed_null collapses to the
     * ordinary empty string and renders as "" — never 0Nc (which the parser
     * cannot even read back). */
    ray_t* result = ray_fmt(ray_typed_null(-RAY_STR), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_STR_EQ("\"\"", s);
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_empty_str_atom(void) {
    /* An explicitly-constructed empty string atom renders "" too. */
    ray_t* result = ray_fmt(ray_str("", 0), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_STR_EQ("\"\"", s);
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_null_guid(void) {
    ray_t* result = ray_fmt(ray_typed_null(-RAY_GUID), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0Ng"));
    ray_release(result);
    PASS();
}

static test_result_t test_fmt_null_sym(void) {
    /* SYM has no null: ray_typed_null(-RAY_SYM) is sym 0 (the empty
     * symbol), which renders as the bare quote literal '. */
    ray_t* result = ray_fmt(ray_typed_null(-RAY_SYM), 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "'"));
    ray_release(result);
    PASS();
}

/* ---- Test: vector types (f32, u8, i16, i32, date, time, timestamp, sym, str, guid) ---- */
static test_result_t test_fmt_vec_f32(void) {
    float raw[] = {1.5f, 2.5f, 3.5f};
    ray_t* vec = ray_vec_from_raw(RAY_F32, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "1.5"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_u8(void) {
    uint8_t raw[] = {0x01, 0x02, 0xFF};
    ray_t* vec = ray_vec_from_raw(RAY_U8, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "0x01"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_i16(void) {
    int16_t raw[] = {100, 200, 300};
    ray_t* vec = ray_vec_from_raw(RAY_I16, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "100"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_i32(void) {
    int32_t raw[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I32, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "10"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_date(void) {
    int32_t raw[] = {0, 1, 365};
    ray_t* vec = ray_vec_from_raw(RAY_DATE, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_time(void) {
    int32_t raw[] = {0, 3600000, -1000};
    ray_t* vec = ray_vec_from_raw(RAY_TIME, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_timestamp(void) {
    int64_t raw[] = {0, (int64_t)86400LL * 1000000000LL};
    ray_t* vec = ray_vec_from_raw(RAY_TIMESTAMP, raw, 2);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_bool(void) {
    bool raw[] = {true, false, true};
    ray_t* vec = ray_vec_from_raw(RAY_BOOL, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "true"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_sym(void) {
    int64_t id_a = ray_sym_intern("alpha", 5);
    int64_t id_b = ray_sym_intern("beta", 4);
    /* use adaptive sym width vec */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_vec_append(vec, &id_a);
    vec = ray_vec_append(vec, &id_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "alpha"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_str(void) {
    ray_t* vec = ray_vec_new(RAY_STR, 2);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, "hello", 5);
    vec = ray_str_vec_append(vec, "world", 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "hello"));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_vec_str_null(void) {
    /* A requested null in a STR vector collapses to the ordinary empty
     * string, so it renders as "" and its null predicate remains false. */
    ray_t* vec = ray_vec_new(RAY_STR, 2);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, "hello", 5);
    vec = ray_str_vec_append(vec, "", 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_vec_set_null(vec, 1, true);
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NULL(strstr(s, "0Nc"));
    TEST_ASSERT_NOT_NULL(strstr(s, "\"\""));
    ray_release(result);
    ray_release(vec);
    PASS();
}

static test_result_t test_fmt_list_empty_strs(void) {
    /* A list holding empty-string atoms renders ("" "x" ""), not (0Nc ...).
     * mode 1 (REPL) renders a generic list with parens, distinct from a vec. */
    ray_t* lst = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(lst);
    lst = ray_list_append(lst, ray_str("", 0));
    lst = ray_list_append(lst, ray_str("x", 1));
    lst = ray_list_append(lst, ray_str("", 0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(lst));
    ray_t* result = ray_fmt(lst, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NULL(strstr(s, "0Nc"));
    TEST_ASSERT_STR_EQ("(\"\" \"x\" \"\")", s);
    ray_release(result);
    ray_release(lst);
    PASS();
}

static test_result_t test_fmt_vec_guid(void) {
    uint8_t g1[16] = {0};
    uint8_t g2[16] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                      0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
    ray_t* vec = ray_vec_new(RAY_GUID, 2);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_vec_append(vec, g1);
    vec = ray_vec_append(vec, g2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strchr(s, '-'));
    ray_release(result);
    ray_release(vec);
    PASS();
}

/* ---- Test: vector width truncation (generates "..]") ---- */
static test_result_t test_fmt_vec_truncate(void) {
    /* narrow width so the vector output truncates */
    ray_fmt_set_width(10);
    int64_t raw[] = {1000000, 2000000, 3000000, 4000000, 5000000, 6000000, 7000000, 8000000};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 8);
    TEST_ASSERT_NOT_NULL(vec);
    ray_t* result = ray_fmt(vec, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "..]"));
    ray_release(result);
    ray_release(vec);
    ray_fmt_set_width(80);
    PASS();
}

/* ---- Test: list formatting (heterogeneous) ---- */
static test_result_t test_fmt_list_hetero(void) {
    ray_t* list = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(list);
    list = ray_list_append(list, ray_i64(1));
    list = ray_list_append(list, ray_f64(2.5));
    list = ray_list_append(list, ray_bool(true));
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    /* mode 1 = REPL display "(..." */
    ray_t* result = ray_fmt(list, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strchr(s, '('));
    ray_release(result);
    /* mode 0 = compact "(list ..." */
    result = ray_fmt(list, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "(list "));
    ray_release(result);
    ray_release(list);
    PASS();
}

static test_result_t test_fmt_list_empty(void) {
    ray_t* list = ray_list_new(0);
    TEST_ASSERT_NOT_NULL(list);
    ray_t* result = ray_fmt(list, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "()"));
    ray_release(result);
    ray_release(list);
    PASS();
}

/* A homogeneous-atom LIST must render with parens so it is distinct from a
 * typed vector — not collapsed to [..] which looks like a vector. */
static test_result_t test_fmt_list_homogeneous_parens(void) {
    ray_t* list = ray_list_new(3);
    list = ray_list_append(list, ray_i64(1));
    list = ray_list_append(list, ray_i64(2));
    list = ray_list_append(list, ray_i64(3));
    /* mode 1 (REPL display): distinct from a vector → parens */
    ray_t* r1 = ray_fmt(list, 1);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_STR_EQ(ray_str_ptr(r1), "(1 2 3)");
    ray_release(r1);
    /* mode 0 (round-trip): legacy homogeneous-list/vector display
     * equivalence preserved — renders as [..] so fmt_eq stays consistent. */
    ray_t* r0 = ray_fmt(list, 0);
    TEST_ASSERT_NOT_NULL(r0);
    TEST_ASSERT_STR_EQ(ray_str_ptr(r0), "[1 2 3]");
    ray_release(r0);
    ray_release(list);
    PASS();
}

/* ---- Test: dict formatting ---- */
static test_result_t test_fmt_dict_sym_i64(void) {
    /* dict with sym keys and i64 vals: {sym: i64 ...} */
    int64_t k1 = ray_sym_intern("a", 1);
    int64_t k2 = ray_sym_intern("b", 1);
    /* Build keys as sym vec */
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 2);
    TEST_ASSERT_NOT_NULL(keys);
    keys = ray_vec_append(keys, &k1);
    keys = ray_vec_append(keys, &k2);
    /* Build vals as i64 vec */
    int64_t raw_v[] = {10, 20};
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    TEST_ASSERT_NOT_NULL(vals);

    /* ray_dict_new consumes both */
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    TEST_ASSERT_FALSE(RAY_IS_ERR(dict));

    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strchr(s, '{'));
    TEST_ASSERT_NOT_NULL(strchr(s, ':'));
    TEST_ASSERT_NOT_NULL(strstr(s, "a:"));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_i64_f64(void) {
    int64_t raw_k[] = {1, 2};
    double  raw_v[] = {1.1, 2.2};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_F64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    TEST_ASSERT_FALSE(RAY_IS_ERR(dict));
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "1:"));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_empty(void) {
    /* empty dict: {} */
    ray_t* keys = ray_vec_new(RAY_I64, 0);
    ray_t* vals = ray_vec_new(RAY_I64, 0);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "{}"));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_str_vals(void) {
    /* dict with i64 keys, str vals */
    int64_t raw_k[] = {1, 2};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    ray_t* vals = ray_vec_new(RAY_STR, 2);
    vals = ray_str_vec_append(vals, "foo", 3);
    vals = ray_str_vec_append(vals, "bar", 3);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    TEST_ASSERT_FALSE(RAY_IS_ERR(dict));
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "foo"));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_i32_vals(void) {
    int64_t raw_k[] = {1, 2};
    int32_t raw_v[] = {100, 200};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I32, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "100"));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_bool_vals(void) {
    int64_t raw_k[] = {1, 2};
    bool raw_v[] = {true, false};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_BOOL, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_i16_key(void) {
    int16_t raw_k[] = {10, 20};
    int64_t raw_v[] = {1, 2};
    ray_t* keys = ray_vec_from_raw(RAY_I16, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_f64_key(void) {
    double raw_k[] = {1.5, 2.5};
    int64_t raw_v[] = {10, 20};
    ray_t* keys = ray_vec_from_raw(RAY_F64, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_date_key(void) {
    int32_t raw_k[] = {0, 1};
    int64_t raw_v[] = {100, 200};
    ray_t* keys = ray_vec_from_raw(RAY_DATE, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_guid_key(void) {
    uint8_t g1[16] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                      0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10};
    uint8_t g2[16] = {0x10,0x0f,0x0e,0x0d,0x0c,0x0b,0x0a,0x09,
                      0x08,0x07,0x06,0x05,0x04,0x03,0x02,0x01};
    ray_t* keys = ray_vec_new(RAY_GUID, 2);
    keys = ray_vec_append(keys, g1);
    keys = ray_vec_append(keys, g2);
    int64_t raw_v[] = {1, 2};
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strchr(s, '-'));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_str_key(void) {
    ray_t* keys = ray_vec_new(RAY_STR, 2);
    keys = ray_str_vec_append(keys, "key1", 4);
    keys = ray_str_vec_append(keys, "key2", 4);
    int64_t raw_v[] = {10, 20};
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "key1"));
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_f32_key(void) {
    float raw_k[] = {1.0f, 2.0f};
    int64_t raw_v[] = {10, 20};
    ray_t* keys = ray_vec_from_raw(RAY_F32, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_i32_key(void) {
    int32_t raw_k[] = {5, 10};
    int64_t raw_v[] = {50, 100};
    ray_t* keys = ray_vec_from_raw(RAY_I32, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_timestamp_key(void) {
    int64_t raw_k[] = {0, (int64_t)86400LL * 1000000000LL};
    int64_t raw_v[] = {1, 2};
    ray_t* keys = ray_vec_from_raw(RAY_TIMESTAMP, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I64, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_sym_vals(void) {
    int64_t raw_k[] = {1, 2};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    int64_t s1 = ray_sym_intern("x", 1);
    int64_t s2 = ray_sym_intern("y", 1);
    ray_t* vals = ray_sym_vec_new(RAY_SYM_W64, 2);
    vals = ray_vec_append(vals, &s1);
    vals = ray_vec_append(vals, &s2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_guid_vals(void) {
    int64_t raw_k[] = {1, 2};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    uint8_t g1[16] = {0};
    uint8_t g2[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    ray_t* vals = ray_vec_new(RAY_GUID, 2);
    vals = ray_vec_append(vals, g1);
    vals = ray_vec_append(vals, g2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_i16_vals(void) {
    int64_t raw_k[] = {1, 2};
    int16_t raw_v[] = {10, 20};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_I16, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

static test_result_t test_fmt_dict_f32_vals(void) {
    int64_t raw_k[] = {1, 2};
    float raw_v[] = {1.5f, 2.5f};
    ray_t* keys = ray_vec_from_raw(RAY_I64, raw_k, 2);
    ray_t* vals = ray_vec_from_raw(RAY_F32, raw_v, 2);
    ray_t* dict = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(dict);
    ray_t* result = ray_fmt(dict, 1);
    TEST_ASSERT_NOT_NULL(result);
    ray_release(result);
    ray_release(dict);
    PASS();
}

/* ---- Test: table in compact mode (mode 0) ---- */
static test_result_t test_fmt_table_mode0(void) {
    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_NOT_NULL(tbl);
    int64_t id_a = ray_sym_intern("a", 1);
    int64_t raw[] = {1, 2};
    ray_t* col = ray_vec_from_raw(RAY_I64, raw, 2);
    tbl = ray_table_add_col(tbl, id_a, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_t* result = ray_fmt(tbl, 0);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "(table"));
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: table with 0 visible columns ("<table>") ---- */
static test_result_t test_fmt_table_empty(void) {
    ray_t* tbl = ray_table_new(0);
    TEST_ASSERT_NOT_NULL(tbl);
    /* mode 1 + table_width==0 => "<table>" */
    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<table>"));
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: table with more than FMT_TABLE_MAX_WIDTH cols (triggers heap alloc + truncation) ---- */
static test_result_t test_fmt_table_wide(void) {
    int64_t ncols = FMT_TABLE_MAX_WIDTH + 2;
    ray_t* tbl = ray_table_new(3);
    TEST_ASSERT_NOT_NULL(tbl);
    for (int64_t i = 0; i < ncols; i++) {
        char name[8];
        snprintf(name, sizeof(name), "c%d", (int)i);
        int64_t id = ray_sym_intern(name, strlen(name));
        int64_t raw[] = {i, i + 1, i + 2};
        ray_t* col = ray_vec_from_raw(RAY_I64, raw, 3);
        tbl = ray_table_add_col(tbl, id, col);
        if (RAY_IS_ERR(tbl)) break;
    }
    /* Even if some cols failed, format what we have */
    if (!RAY_IS_ERR(tbl)) {
        ray_t* result = ray_fmt(tbl, 1);
        TEST_ASSERT_NOT_NULL(result);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        const char* s = ray_str_ptr(result);
        /* Wide table should contain the truncation indicator */
        TEST_ASSERT_NOT_NULL(s);
        ray_release(result);
    }
    ray_release(tbl);
    PASS();
}

/* ---- Test: table with more than FMT_TABLE_MAX_HEIGHT rows (triggers row truncation) ---- */
static test_result_t test_fmt_table_tall(void) {
    int64_t nrows = FMT_TABLE_MAX_HEIGHT + 5;
    int64_t id_v = ray_sym_intern("v", 1);
    ray_t* col = ray_vec_new(RAY_I64, nrows);
    TEST_ASSERT_NOT_NULL(col);
    for (int64_t i = 0; i < nrows; i++) {
        col = ray_vec_append(col, &i);
        if (RAY_IS_ERR(col)) { ray_release(col); PASS(); }
    }
    ray_t* tbl = ray_table_new((int32_t)nrows);
    tbl = ray_table_add_col(tbl, id_v, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_t* result = ray_fmt(tbl, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    /* Should contain "rows" footer */
    TEST_ASSERT_NOT_NULL(strstr(s, "rows"));
    ray_release(result);
    ray_release(tbl);
    PASS();
}

/* ---- Test: ray_fmt_print ---- */
static test_result_t test_fmt_print(void) {
    ray_t* obj = ray_i64(42);
    /* just verify it doesn't crash */
    ray_fmt_print(stdout, obj, 1);
    ray_release(obj);
    PASS();
}

/* ---- Test: format builtin functions (unary/binary/vary) ---- */
static ray_t* dummy_unary_fn(ray_t* x) { (void)x; return ray_i64(0); }
static ray_t* dummy_binary_fn(ray_t* x, ray_t* y) { (void)x; (void)y; return ray_i64(0); }
static ray_t* dummy_vary_fn(ray_t** args, int64_t n) { (void)args; (void)n; return ray_i64(0); }

static test_result_t test_fmt_fn_unary(void) {
    ray_t* fn = ray_fn_unary("neg", RAY_FN_ATOMIC, dummy_unary_fn);
    TEST_ASSERT_NOT_NULL(fn);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<neg>"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

static test_result_t test_fmt_fn_unary_noname(void) {
    ray_t* fn = ray_fn_unary("", RAY_FN_ATOMIC, dummy_unary_fn);
    TEST_ASSERT_NOT_NULL(fn);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<builtin/1>"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

static test_result_t test_fmt_fn_binary(void) {
    ray_t* fn = ray_fn_binary("add", RAY_FN_ATOMIC, dummy_binary_fn);
    TEST_ASSERT_NOT_NULL(fn);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<add>"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

static test_result_t test_fmt_fn_binary_noname(void) {
    ray_t* fn = ray_fn_binary("", RAY_FN_ATOMIC, dummy_binary_fn);
    TEST_ASSERT_NOT_NULL(fn);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<builtin/2>"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

static test_result_t test_fmt_fn_vary(void) {
    ray_t* fn = ray_fn_vary("list", RAY_FN_NONE, dummy_vary_fn);
    TEST_ASSERT_NOT_NULL(fn);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<list>"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

static test_result_t test_fmt_fn_vary_noname(void) {
    ray_t* fn = ray_fn_vary("", RAY_FN_NONE, dummy_vary_fn);
    TEST_ASSERT_NOT_NULL(fn);
    ray_t* result = ray_fmt(fn, 1);
    TEST_ASSERT_NOT_NULL(result);
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "<builtin/n>"));
    ray_release(result);
    ray_release(fn);
    PASS();
}

/* ---- Test: ray_type_name for dict, list, str, date, time, timestamp, guid ---- */
static test_result_t test_type_name_all(void) {
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_DICT),  "DICT");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_LIST),  "LIST");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_STR),   "STR");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_STR),  "str");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_GUID),  "GUID");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_GUID), "guid");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_DATE),  "DATE");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_DATE), "date");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_TIME),  "TIME");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_TIME), "time");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_TIMESTAMP), "TIMESTAMP");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_TIMESTAMP), "timestamp");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_BOOL),  "B8");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_BOOL), "b8");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_U8),    "U8");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_U8),   "u8");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_I16),   "I16");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_I16),  "i16");
    TEST_ASSERT_STR_EQ(ray_type_name(RAY_I32),   "I32");
    TEST_ASSERT_STR_EQ(ray_type_name(-RAY_I32),  "i32");
    PASS();
}

/* ---- Test: format null atom (RAY_IS_NULL obj) ---- */
static test_result_t test_fmt_null_obj(void) {
    /* ray_typed_null with type 0 = RAY_LIST null → "null" */
    ray_t* result = ray_fmt(NULL, 1);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    const char* s = ray_str_ptr(result);
    TEST_ASSERT_NOT_NULL(strstr(s, "null"));
    ray_release(result);
    PASS();
}

/* ---- Pretty-printer (ray_fmt_pp) tests ---- */

/* Helper: eval src, pretty-print it, compare to expected, then reset width. */
static int pp_eq(const char* src, const char* expected) {
    ray_t* v = ray_eval_str(src);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return 0; }
    ray_t* s = ray_fmt_pp(v);
    int ok = s && !RAY_IS_ERR(s)
          && strlen(expected) == (size_t)ray_str_len(s)
          && memcmp(expected, ray_str_ptr(s), ray_str_len(s)) == 0;
    if (s) ray_release(s);
    ray_release(v);
    return ok;
}

/* A small dict that fits the width stays on one line. */
static test_result_t test_fmt_pp_dict_fits(void) {
    ray_fmt_set_width(80);
    TEST_ASSERT_TRUE(pp_eq("(dict [a b] (list 1 2))", "{a:1 b:2}"));
    PASS();
}

/* A dict wider than the limit breaks one pair per line, indented 2 spaces. */
static test_result_t test_fmt_pp_dict_breaks(void) {
    ray_fmt_set_width(10);
    int ok = pp_eq("(dict [a b c] (list 1 2 3))",
                   "{\n  a: 1\n  b: 2\n  c: 3\n}");
    ray_fmt_set_width(80);
    TEST_ASSERT_TRUE(ok);
    PASS();
}

/* When the outer breaks but a nested dict still fits on its line, the nested
 * one stays compact (JSON-pp behavior). */
static test_result_t test_fmt_pp_nested_compact(void) {
    ray_fmt_set_width(12);
    int ok = pp_eq("(dict [a b c] (list (dict [x] (list 1)) 2 3))",
                   "{\n  a: {x:1}\n  b: 2\n  c: 3\n}");
    ray_fmt_set_width(80);
    TEST_ASSERT_TRUE(ok);
    PASS();
}

/* A list that fits renders compact with parens (distinct from a vector). */
static test_result_t test_fmt_pp_list_fits(void) {
    ray_fmt_set_width(80);
    TEST_ASSERT_TRUE(pp_eq("(list 1 (list 2 3))", "(1 (2 3))"));
    PASS();
}

/* Scalars and vectors pretty-print exactly as ray_fmt mode 1 (no breaking). */
static test_result_t test_fmt_pp_scalar_passthrough(void) {
    ray_fmt_set_width(80);
    TEST_ASSERT_TRUE(pp_eq("42", "42"));
    TEST_ASSERT_TRUE(pp_eq("[1 2 3]", "[1 2 3]"));
    PASS();
}

/* ---- Suite definition ---- */

const test_entry_t format_entries[] = {
    { "format/pp/dict_fits", test_fmt_pp_dict_fits, fmt_setup_full, fmt_teardown_full },
    { "format/pp/dict_breaks", test_fmt_pp_dict_breaks, fmt_setup_full, fmt_teardown_full },
    { "format/pp/nested_compact", test_fmt_pp_nested_compact, fmt_setup_full, fmt_teardown_full },
    { "format/pp/list_fits", test_fmt_pp_list_fits, fmt_setup_full, fmt_teardown_full },
    { "format/pp/scalar_passthrough", test_fmt_pp_scalar_passthrough, fmt_setup_full, fmt_teardown_full },
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
    /* New tests */
    { "format/settings/precision", test_fmt_set_precision, fmt_setup, fmt_teardown },
    { "format/settings/width", test_fmt_set_width, fmt_setup, fmt_teardown },
    { "format/type/f32", test_type_name_f32, fmt_setup, fmt_teardown },
    { "format/type/index", test_type_name_index, fmt_setup, fmt_teardown },
    { "format/type/unknown", test_type_name_unknown, fmt_setup, fmt_teardown },
    { "format/type/all", test_type_name_all, fmt_setup, fmt_teardown },
    { "format/atom/u8", test_fmt_atom_u8, fmt_setup, fmt_teardown },
    { "format/atom/i16", test_fmt_atom_i16, fmt_setup, fmt_teardown },
    { "format/atom/i32", test_fmt_atom_i32, fmt_setup, fmt_teardown },
    { "format/atom/f32", test_fmt_atom_f32, fmt_setup, fmt_teardown },
    { "format/atom/date", test_fmt_atom_date, fmt_setup, fmt_teardown },
    { "format/atom/time", test_fmt_atom_time, fmt_setup, fmt_teardown },
    { "format/atom/time_neg", test_fmt_atom_time_neg, fmt_setup, fmt_teardown },
    { "format/atom/timestamp", test_fmt_atom_timestamp, fmt_setup, fmt_teardown },
    { "format/atom/sym", test_fmt_atom_sym, fmt_setup, fmt_teardown },
    { "format/atom/str", test_fmt_atom_str, fmt_setup, fmt_teardown },
    { "format/atom/guid", test_fmt_atom_guid, fmt_setup, fmt_teardown },
    { "format/null/bool", test_fmt_null_bool, fmt_setup, fmt_teardown },
    { "format/null/u8", test_fmt_null_u8, fmt_setup, fmt_teardown },
    { "format/null/f32", test_fmt_null_f32, fmt_setup, fmt_teardown },
    { "format/null/date", test_fmt_null_date, fmt_setup, fmt_teardown },
    { "format/null/str", test_fmt_null_str, fmt_setup, fmt_teardown },
    { "format/atom/empty_str", test_fmt_empty_str_atom, fmt_setup, fmt_teardown },
    { "format/null/guid", test_fmt_null_guid, fmt_setup, fmt_teardown },
    { "format/null/sym", test_fmt_null_sym, fmt_setup, fmt_teardown },
    { "format/null/obj", test_fmt_null_obj, fmt_setup, fmt_teardown },
    { "format/vec/f32", test_fmt_vec_f32, fmt_setup, fmt_teardown },
    { "format/vec/u8", test_fmt_vec_u8, fmt_setup, fmt_teardown },
    { "format/vec/i16", test_fmt_vec_i16, fmt_setup, fmt_teardown },
    { "format/vec/i32", test_fmt_vec_i32, fmt_setup, fmt_teardown },
    { "format/vec/date", test_fmt_vec_date, fmt_setup, fmt_teardown },
    { "format/vec/time", test_fmt_vec_time, fmt_setup, fmt_teardown },
    { "format/vec/timestamp", test_fmt_vec_timestamp, fmt_setup, fmt_teardown },
    { "format/vec/bool", test_fmt_vec_bool, fmt_setup, fmt_teardown },
    { "format/vec/sym", test_fmt_vec_sym, fmt_setup, fmt_teardown },
    { "format/vec/str", test_fmt_vec_str, fmt_setup, fmt_teardown },
    { "format/vec/str_null", test_fmt_vec_str_null, fmt_setup, fmt_teardown },
    { "format/list/empty_strs", test_fmt_list_empty_strs, fmt_setup, fmt_teardown },
    { "format/vec/guid", test_fmt_vec_guid, fmt_setup, fmt_teardown },
    { "format/vec/truncate", test_fmt_vec_truncate, fmt_setup, fmt_teardown },
    { "format/list/hetero", test_fmt_list_hetero, fmt_setup, fmt_teardown },
    { "format/list/empty", test_fmt_list_empty, fmt_setup, fmt_teardown },
    { "format/list/homogeneous_parens", test_fmt_list_homogeneous_parens, fmt_setup, fmt_teardown },
    { "format/dict/sym_i64", test_fmt_dict_sym_i64, fmt_setup, fmt_teardown },
    { "format/dict/i64_f64", test_fmt_dict_i64_f64, fmt_setup, fmt_teardown },
    { "format/dict/empty", test_fmt_dict_empty, fmt_setup, fmt_teardown },
    { "format/dict/str_vals", test_fmt_dict_str_vals, fmt_setup, fmt_teardown },
    { "format/dict/i32_vals", test_fmt_dict_i32_vals, fmt_setup, fmt_teardown },
    { "format/dict/bool_vals", test_fmt_dict_bool_vals, fmt_setup, fmt_teardown },
    { "format/dict/i16_key", test_fmt_dict_i16_key, fmt_setup, fmt_teardown },
    { "format/dict/f64_key", test_fmt_dict_f64_key, fmt_setup, fmt_teardown },
    { "format/dict/date_key", test_fmt_dict_date_key, fmt_setup, fmt_teardown },
    { "format/dict/guid_key", test_fmt_dict_guid_key, fmt_setup, fmt_teardown },
    { "format/dict/str_key", test_fmt_dict_str_key, fmt_setup, fmt_teardown },
    { "format/dict/f32_key", test_fmt_dict_f32_key, fmt_setup, fmt_teardown },
    { "format/dict/i32_key", test_fmt_dict_i32_key, fmt_setup, fmt_teardown },
    { "format/dict/timestamp_key", test_fmt_dict_timestamp_key, fmt_setup, fmt_teardown },
    { "format/dict/sym_vals", test_fmt_dict_sym_vals, fmt_setup, fmt_teardown },
    { "format/dict/guid_vals", test_fmt_dict_guid_vals, fmt_setup, fmt_teardown },
    { "format/dict/i16_vals", test_fmt_dict_i16_vals, fmt_setup, fmt_teardown },
    { "format/dict/f32_vals", test_fmt_dict_f32_vals, fmt_setup, fmt_teardown },
    { "format/table/mode0", test_fmt_table_mode0, fmt_setup, fmt_teardown },
    { "format/table/empty", test_fmt_table_empty, fmt_setup, fmt_teardown },
    { "format/table/wide", test_fmt_table_wide, fmt_setup, fmt_teardown },
    { "format/table/tall", test_fmt_table_tall, fmt_setup, fmt_teardown },
    { "format/print", test_fmt_print, fmt_setup, fmt_teardown },
    { "format/fn/unary", test_fmt_fn_unary, fmt_setup_full, fmt_teardown_full },
    { "format/fn/unary_noname", test_fmt_fn_unary_noname, fmt_setup_full, fmt_teardown_full },
    { "format/fn/binary", test_fmt_fn_binary, fmt_setup_full, fmt_teardown_full },
    { "format/fn/binary_noname", test_fmt_fn_binary_noname, fmt_setup_full, fmt_teardown_full },
    { "format/fn/vary", test_fmt_fn_vary, fmt_setup_full, fmt_teardown_full },
    { "format/fn/vary_noname", test_fmt_fn_vary_noname, fmt_setup_full, fmt_teardown_full },
    /* Additional edge case tests */
    { "format/sym/invalid", test_fmt_sym_invalid, fmt_setup, fmt_teardown },
    { "format/null/default", test_fmt_null_default, fmt_setup, fmt_teardown },
    { "format/lambda", test_fmt_lambda, fmt_setup_full, fmt_teardown_full },
    { "format/table/list_col", test_fmt_table_list_col, fmt_setup, fmt_teardown },
    { "format/table/mode2", test_fmt_table_mode2, fmt_setup, fmt_teardown },
    { "format/table/short_col", test_fmt_table_short_col, fmt_setup, fmt_teardown },
    { "format/null/table_atom", test_fmt_null_table_atom, fmt_setup, fmt_teardown },
    { "format/table/wide_tall", test_fmt_table_wide_and_tall, fmt_setup, fmt_teardown },
    { "format/table/list_col_null", test_fmt_raw_elem_list_null, fmt_setup, fmt_teardown },
    { "format/table/na_head", test_fmt_table_na_head, fmt_setup, fmt_teardown },
    { NULL, NULL, NULL, NULL },
};

