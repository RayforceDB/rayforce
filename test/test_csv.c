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
#include "io/csv.h"
#include <stdio.h>
#include <unistd.h>

static char tmp_csv_path[64];
static const char* tmp_csv(void) {
    if (!tmp_csv_path[0])
        snprintf(tmp_csv_path, sizeof(tmp_csv_path),
                 "/tmp/rayforce_test_%d.csv", (int)getpid());
    return tmp_csv_path;
}
#define TMP_CSV tmp_csv()

static test_result_t test_csv_roundtrip_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t vals[] = {10, 20, 30};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    int64_t name = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 1);

    /* Verify actual data values survived the roundtrip */
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_NOT_NULL(col);
    int64_t* loaded_data = (int64_t*)ray_data(col);
    TEST_ASSERT_EQ_I(loaded_data[0], 10);
    TEST_ASSERT_EQ_I(loaded_data[1], 20);
    TEST_ASSERT_EQ_I(loaded_data[2], 30);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_roundtrip_f64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    double vals[] = {1.5, 2.5, 3.5};
    ray_t* vec = ray_vec_from_raw(RAY_F64, vals, 3);
    int64_t name = ray_sym_intern("price", 5);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    /* Verify F64 values survived the roundtrip */
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_I(col->type, RAY_F64);
    double* loaded_data = (double*)ray_data(col);
    TEST_ASSERT_EQ_F(loaded_data[0], 1.5, 1e-6);
    TEST_ASSERT_EQ_F(loaded_data[1], 2.5, 1e-6);
    TEST_ASSERT_EQ_F(loaded_data[2], 3.5, 1e-6);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_multi_column(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t ids[] = {1, 2, 3};
    double vals[] = {10.5, 20.5, 30.5};
    ray_t* id_v = ray_vec_from_raw(RAY_I64, ids, 3);
    ray_t* val_v = ray_vec_from_raw(RAY_F64, vals, 3);
    int64_t n_id = ray_sym_intern("id", 2);
    int64_t n_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_id, id_v);
    tbl = ray_table_add_col(tbl, n_val, val_v);
    ray_release(id_v);
    ray_release(val_v);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    /* Verify both columns' data values */
    ray_t* id_col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_NOT_NULL(id_col);
    int64_t* id_data = (int64_t*)ray_data(id_col);
    TEST_ASSERT_EQ_I(id_data[0], 1);
    TEST_ASSERT_EQ_I(id_data[1], 2);
    TEST_ASSERT_EQ_I(id_data[2], 3);
    ray_t* val_col = ray_table_get_col_idx(loaded, 1);
    TEST_ASSERT_NOT_NULL(val_col);
    double* val_data = (double*)ray_data(val_col);
    TEST_ASSERT_EQ_F(val_data[0], 10.5, 1e-6);
    TEST_ASSERT_EQ_F(val_data[1], 20.5, 1e-6);
    TEST_ASSERT_EQ_F(val_data[2], 30.5, 1e-6);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_empty_table(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* tbl = ray_table_new(0);
    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    /* Empty table (0 cols) should return RAY_ERR_TYPE */
    TEST_ASSERT_EQ_I(err, RAY_ERR_TYPE);

    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_null_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\n\n30\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_NOT_NULL(col);

    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[0], 10);
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[2], 30);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_null_i64_unparseable(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\nN/A\n30\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[0], 10);
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[2], 30);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_null_f64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n1.5\n\n3.5\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_EQ_F(((double*)ray_data(col))[0], 1.5, 1e-6);
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));
    TEST_ASSERT_EQ_F(((double*)ray_data(col))[2], 3.5, 1e-6);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_null_bool(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "flag\ntrue\n\nfalse\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_EQ_I((int)((uint8_t*)ray_data(col))[0], 1);
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));  /* empty */
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));
    TEST_ASSERT_EQ_I((int)((uint8_t*)ray_data(col))[2], 0);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_null_sym(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "name\nalice\n\nbob\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));  /* empty → NULL */
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_no_nulls_no_nullmap(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\n20\n30\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    /* No nulls → HAS_NULLS flag should be stripped */
    TEST_ASSERT_FALSE(col->attrs & RAY_ATTR_HAS_NULLS);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_null_mixed_columns(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "id,val,name\n1,1.5,alice\n,2.5,\n3,,bob\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 3);

    ray_t* id_col = ray_table_get_col_idx(loaded, 0);
    ray_t* val_col = ray_table_get_col_idx(loaded, 1);
    ray_t* name_col = ray_table_get_col_idx(loaded, 2);

    /* id column: 1, NULL, 3 */
    TEST_ASSERT_FALSE(ray_vec_is_null(id_col, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(id_col, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(id_col, 2));

    /* val column: 1.5, 2.5, NULL */
    TEST_ASSERT_FALSE(ray_vec_is_null(val_col, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(val_col, 1));
    TEST_ASSERT_TRUE(ray_vec_is_null(val_col, 2));

    /* name column: alice, NULL, bob */
    TEST_ASSERT_FALSE(ray_vec_is_null(name_col, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(name_col, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(name_col, 2));

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Explicit RAY_STR schema must yield a RAY_STR vector, not a sym column.
 * Regression: csv loader used to funnel all string-parsed columns through
 * the sym intern table, silently corrupting RAY_STR-typed outputs. */
static test_result_t test_csv_explicit_str_schema(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    /* Mix inline (<=12B), pooled (>12B), empty/null, and a short */
    fprintf(f, "id,note\n"
               "1,hi\n"
               "2,this-is-a-long-string-over-12-bytes\n"
               "3,\n"
               "4,tiny\n");
    fclose(f);

    int8_t schema[2] = { RAY_I64, RAY_STR };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 4);

    ray_t* note = ray_table_get_col_idx(loaded, 1);
    TEST_ASSERT_EQ_I(note->type, RAY_STR);

    size_t l;
    const char* s;
    s = ray_str_vec_get(note, 0, &l);
    TEST_ASSERT_EQ_I((int)l, 2);
    TEST_ASSERT_MEM_EQ(2, s, "hi");
    s = ray_str_vec_get(note, 1, &l);
    TEST_ASSERT_EQ_I((int)l, 35);
    TEST_ASSERT_MEM_EQ(35, s, "this-is-a-long-string-over-12-bytes");
    TEST_ASSERT_TRUE(ray_vec_is_null(note, 2));
    s = ray_str_vec_get(note, 3, &l);
    TEST_ASSERT_EQ_I((int)l, 4);
    TEST_ASSERT_MEM_EQ(4, s, "tiny");

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Round-trip for strings containing commas, quotes, and newlines.
 * Regression test: escaped fields (with "") were stored as pointers into
 * a stack-local esc_buf that died before csv_fill_str_cols could read them,
 * causing use-after-return. */
static test_result_t test_csv_escaped_str_roundtrip(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Write a CSV with fields that require quoting/escaping */
    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "s\n"
               "\"he,llo\"\n"
               "\"wo\"\"rld\"\n"
               "plain\n"
               "\"line1\nline2\"\n");
    fclose(f);

    /* Read back as RAY_STR */
    int8_t schema[1] = { RAY_STR };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 4);

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_STR);

    size_t l;
    const char* s;

    s = ray_str_vec_get(col, 0, &l);
    TEST_ASSERT_EQ_I((int)l, 6);
    TEST_ASSERT_MEM_EQ(6, s, "he,llo");

    s = ray_str_vec_get(col, 1, &l);
    TEST_ASSERT_EQ_I((int)l, 6);
    TEST_ASSERT_MEM_EQ(6, s, "wo\"rld");

    s = ray_str_vec_get(col, 2, &l);
    TEST_ASSERT_EQ_I((int)l, 5);
    TEST_ASSERT_MEM_EQ(5, s, "plain");

    s = ray_str_vec_get(col, 3, &l);
    TEST_ASSERT_EQ_I((int)l, 11);
    TEST_ASSERT_MEM_EQ(11, s, "line1\nline2");

    /* Write it back out and verify byte-identical CSV */
    const char* tmp2 = "/tmp/rayforce_test_esc2.csv";
    ray_err_t err = ray_write_csv(loaded, tmp2);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Re-read and compare */
    ray_t* loaded2 = ray_read_csv_opts(tmp2, 0, true, schema, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded2));
    ray_t* col2 = ray_table_get_col_idx(loaded2, 0);
    for (int64_t r = 0; r < 4; r++) {
        size_t l1, l2;
        const char* s1 = ray_str_vec_get(col, r, &l1);
        const char* s2 = ray_str_vec_get(col2, r, &l2);
        TEST_ASSERT_EQ_I((int)l1, (int)l2);
        TEST_ASSERT_MEM_EQ(l1, s1, s2);
    }

    ray_release(loaded2);
    unlink(tmp2);
    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Regression: GUID columns used to fall through the schema map to the SYM
 * pipeline, so a round-trip through CSV produced a column filled with sym
 * IDs reinterpreted as 16-byte GUIDs (see flips.rfl example).  After the
 * dedicated CSV_TYPE_GUID path, a written GUID must read back identically. */
static test_result_t test_csv_guid_roundtrip(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 4 deterministic 16-byte GUIDs, written canonical 8-4-4-4-12 form. */
    uint8_t guids[4][16];
    for (int r = 0; r < 4; r++)
        for (int b = 0; b < 16; b++)
            guids[r][b] = (uint8_t)(0x10 + r * 16 + b);

    ray_t* vec = ray_vec_from_raw(RAY_GUID, guids, 4);
    int64_t name = ray_sym_intern("g", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, name, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    int8_t schema[1] = { RAY_GUID };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 4);

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_GUID);
    const uint8_t* d = (const uint8_t*)ray_data(col);
    for (int r = 0; r < 4; r++) {
        for (int b = 0; b < 16; b++) {
            TEST_ASSERT_FMT(d[r * 16 + b] == guids[r][b],
                "row %d byte %d: got 0x%02x, want 0x%02x",
                r, b, d[r * 16 + b], guids[r][b]);
        }
    }

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t csv_entries[] = {
    { "csv/roundtrip_i64", test_csv_roundtrip_i64, NULL, NULL },
    { "csv/roundtrip_guid", test_csv_guid_roundtrip, NULL, NULL },
    { "csv/roundtrip_f64", test_csv_roundtrip_f64, NULL, NULL },
    { "csv/multi_column", test_csv_multi_column, NULL, NULL },
    { "csv/empty_table", test_csv_empty_table, NULL, NULL },
    { "csv/null_i64", test_csv_null_i64, NULL, NULL },
    { "csv/null_i64_unparseable", test_csv_null_i64_unparseable, NULL, NULL },
    { "csv/null_f64", test_csv_null_f64, NULL, NULL },
    { "csv/null_bool", test_csv_null_bool, NULL, NULL },
    { "csv/null_sym", test_csv_null_sym, NULL, NULL },
    { "csv/no_nulls_no_nullmap", test_csv_no_nulls_no_nullmap, NULL, NULL },
    { "csv/null_mixed_columns", test_csv_null_mixed_columns, NULL, NULL },
    { "csv/explicit_str_schema", test_csv_explicit_str_schema, NULL, NULL },
    { "csv/escaped_str_roundtrip", test_csv_escaped_str_roundtrip, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


