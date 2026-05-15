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
#include "table/sym.h"
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
    /* CSV format conflates "empty field" and "missing field" — both
     * appear as a zero-length cell.  The Rayforce loader interns empty
     * SYM cells as the empty SYM (not the null sentinel) so SQL-style
     * `(!= col "")` filters work the way users expect.  RAY_STR columns
     * and non-string types preserve the null distinction. */
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "name\nalice\n\nbob\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 1));  /* empty → empty SYM, not null */
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));

    /* Row 1's SYM ID resolves to a zero-length string — the empty SYM.
     * The CSV loader narrows SYM columns to W8/W16/W32 based on max ID,
     * so use ray_read_sym instead of a fixed-width cast. */
    int64_t id1 = ray_read_sym(ray_data(col), 1, col->type, col->attrs);
    ray_t* s = ray_sym_str(id1);
    TEST_ASSERT_FALSE(s == NULL);
    TEST_ASSERT_EQ_I((int64_t)ray_str_len(s), 0);

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

    /* name column: alice, "", bob — empty SYM cell becomes the empty
     * SYM (not null).  See test_csv_null_sym for the rationale. */
    TEST_ASSERT_FALSE(ray_vec_is_null(name_col, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(name_col, 1));
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

/* ------------------------------------------------------------------
 * Coverage extension tests (pass-7+): exercise type-specific parse
 * paths, error returns, header inference, and write-side branches
 * to lift csv.c above 80% line coverage.
 * ------------------------------------------------------------------ */

/* Date-only inference path (exactly 10 chars, YYYY-MM-DD). */
static test_result_t test_csv_infer_date(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "d\n2025-01-02\n2026-12-31\n2000-03-15\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_DATE);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Time-only inference path: HH:MM:SS plus optional fraction. */
static test_result_t test_csv_infer_time(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "t\n12:34:56\n00:00:00\n23:59:59.123\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_TIME);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Timestamp inference (YYYY-MM-DD{T| }HH:MM:SS). DATE+TIMESTAMP -> TIMESTAMP. */
static test_result_t test_csv_infer_timestamp_promotion(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Mix of full timestamps with both 'T' and ' ' separators, plus a
     * date-only sentinel that should be promoted to TIMESTAMP. */
    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "ts\n2025-01-02T03:04:05\n2025-06-07 08:09:10.123456\n2024-12-31\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_TIMESTAMP);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Bool inference path (pure true/false rows -> RAY_BOOL). */
static test_result_t test_csv_infer_bool(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "b\ntrue\nfalse\nTRUE\nFALSE\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_BOOL);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* F64 inference via NaN, Inf, exponential, signed-inf literals. */
static test_result_t test_csv_infer_f64_specials(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "v\n1.0\n2e10\n-3.5E-2\nnan\nInf\n+inf\n-INF\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_F64);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Null-sentinel forms recognised by detect_type: N/A, NA, null, None, ".". */
static test_result_t test_csv_infer_null_sentinels(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Sentinel rows alternating with i64 values; column should infer I64. */
    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n10\nN/A\nNA\nnull\nNULL\nNone\nnone\nn/a\nna\n.\n42\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_I64);
    /* Most rows should be null. */
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));   /* 10 */
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));    /* N/A */
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 9));    /* . */
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 10));  /* 42 */

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Promotion: I64 + F64 -> F64; BOOL + I64 -> I64. */
static test_result_t test_csv_infer_promotions(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "n,b\n1,true\n2,0\n3.5,1\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* n = ray_table_get_col_idx(loaded, 0);
    ray_t* b = ray_table_get_col_idx(loaded, 1);
    TEST_ASSERT_EQ_I(n->type, RAY_F64);   /* I64 + F64 -> F64 */
    TEST_ASSERT_EQ_I(b->type, RAY_I64);   /* BOOL + I64 -> I64 */

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Tab-delimiter auto-detection (more tabs than commas in header). */
static test_result_t test_csv_tab_delimiter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "a\tb\tc\n1\t2\t3\n4\t5\t6\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 3);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 2);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* header=false path: synthetic V1, V2, ... names. */
static test_result_t test_csv_no_header(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "10,20\n30,40\n50,60\n");
    fclose(f);

    ray_t* loaded = ray_read_csv_opts(TMP_CSV, ',', false, NULL, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_I64);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Read of a non-existent path returns an error. */
static test_result_t test_csv_read_missing_file(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* loaded = ray_read_csv("/tmp/__rf_csv_does_not_exist_xyz__.csv");
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded));

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ray_write_csv to an unwritable path returns RAY_ERR_IO. */
static test_result_t test_csv_write_bad_path(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t vals[] = {1, 2};
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 2);
    int64_t nm = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, vec);
    ray_release(vec);

    /* Directory that doesn't exist -> fopen of tmp_path fails. */
    ray_err_t err = ray_write_csv(tbl, "/tmp/__nonexistent_dir__/out.csv");
    TEST_ASSERT_EQ_I(err, RAY_ERR_IO);

    /* NULL table / empty path -> RAY_ERR_TYPE. */
    TEST_ASSERT_EQ_I(ray_write_csv(NULL, TMP_CSV), RAY_ERR_TYPE);
    TEST_ASSERT_EQ_I(ray_write_csv(tbl, ""),       RAY_ERR_TYPE);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Explicit schema with an invalid type code returns an error. */
static test_result_t test_csv_invalid_schema_type(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "x\n1\n2\n");
    fclose(f);

    int8_t bad[1] = { (int8_t)RAY_TABLE };  /* table not allowed as col type */
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, ',', true, bad, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded));

    int8_t bad2[1] = { 99 };  /* >= RAY_TYPE_COUNT */
    ray_t* loaded2 = ray_read_csv_opts(TMP_CSV, ',', true, bad2, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded2));

    /* Schema too short for ncols also errors out. */
    int8_t one_only[1] = { RAY_I64 };
    FILE* g = fopen(TMP_CSV, "w");
    fprintf(g, "a,b\n1,2\n");
    fclose(g);
    ray_t* loaded3 = ray_read_csv_opts(TMP_CSV, ',', true, one_only, 1);
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded3));

    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* CRLF line endings are accepted and trailing \r stripped from last field. */
static test_result_t test_csv_crlf_line_endings(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "wb");
    fprintf(f, "a,b\r\n1,2\r\n3,4\r\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 2);
    ray_t* b = ray_table_get_col_idx(loaded, 1);
    TEST_ASSERT_EQ_I(b->type, RAY_I64);
    int64_t* bd = (int64_t*)ray_data(b);
    TEST_ASSERT_EQ_I(bd[0], 2);
    TEST_ASSERT_EQ_I(bd[1], 4);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Truncated row (fewer fields than columns) -> remaining columns null. */
static test_result_t test_csv_truncated_row(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "a,b,c\n1,2,3\n4\n7,8,9\n");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);
    ray_t* b = ray_table_get_col_idx(loaded, 1);
    ray_t* c = ray_table_get_col_idx(loaded, 2);
    /* Row 1: only 'a' supplied -> b and c are null. */
    TEST_ASSERT_TRUE(ray_vec_is_null(b, 1));
    TEST_ASSERT_TRUE(ray_vec_is_null(c, 1));
    /* Other rows intact. */
    TEST_ASSERT_FALSE(ray_vec_is_null(b, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(c, 2));

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Roundtrip RAY_DATE / RAY_TIME / RAY_TIMESTAMP via write -> read. */
static test_result_t test_csv_roundtrip_date_time_ts(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* DATE: int32 days since 2000-01-01 */
    int32_t dates[3] = { 0, 366, 9000 };
    /* TIME: int32 ms since midnight; one with fractional, one negative. */
    int32_t times[3] = { 12 * 3600000, -3600000, 23 * 3600000 + 59 * 60000 + 59 * 1000 + 250 };
    /* TIMESTAMP: int64 ns since 2000-01-01. */
    int64_t tss[3] = { 0, 86400000000000LL, 86400000000000LL + 12345LL };

    ray_t* d_v  = ray_vec_from_raw(RAY_DATE, dates, 3);
    ray_t* t_v  = ray_vec_from_raw(RAY_TIME, times, 3);
    ray_t* ts_v = ray_vec_from_raw(RAY_TIMESTAMP, tss, 3);
    int64_t n_d  = ray_sym_intern("d",  1);
    int64_t n_t  = ray_sym_intern("t",  1);
    int64_t n_ts = ray_sym_intern("ts", 2);

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, n_d,  d_v);
    tbl = ray_table_add_col(tbl, n_t,  t_v);
    tbl = ray_table_add_col(tbl, n_ts, ts_v);
    ray_release(d_v); ray_release(t_v); ray_release(ts_v);

    ray_err_t werr = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(werr, RAY_OK);

    int8_t schema[3] = { RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, ',', true, schema, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    ray_t* dc  = ray_table_get_col_idx(loaded, 0);
    ray_t* tc  = ray_table_get_col_idx(loaded, 1);
    ray_t* tsc = ray_table_get_col_idx(loaded, 2);
    TEST_ASSERT_EQ_I(dc->type,  RAY_DATE);
    TEST_ASSERT_EQ_I(tc->type,  RAY_TIME);
    TEST_ASSERT_EQ_I(tsc->type, RAY_TIMESTAMP);

    /* DATE values must round-trip exactly. */
    int32_t* d2 = (int32_t*)ray_data(dc);
    for (int i = 0; i < 3; i++) TEST_ASSERT_EQ_I(d2[i], dates[i]);

    /* Positive TIME values must round-trip exactly. Negative time is
     * written as "-HH:MM:SS" by csv_write_time, but fast_time only
     * accepts unsigned HH:MM:SS, so the negative cell parses as null.
     * This is a known source limitation (no src/ changes allowed). */
    int32_t* t2 = (int32_t*)ray_data(tc);
    TEST_ASSERT_EQ_I(t2[0], times[0]);
    TEST_ASSERT_TRUE(ray_vec_is_null(tc, 1));   /* negative time → null on read-back */
    TEST_ASSERT_EQ_I(t2[2], times[2]);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Write-side: RAY_I32 / RAY_I16 / RAY_U8 / RAY_F64 (NaN, +inf, -inf). */
static test_result_t test_csv_write_int_widths_and_floats(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int32_t i32v[3] = { -1, 0, 100000 };
    int16_t i16v[3] = { -1, 0, 32000 };
    uint8_t u8v[3]  = { 0, 1, 255 };
    double  fv[3]   = { 0.0/0.0,  1.0/0.0, -1.0/0.0 };  /* nan, +inf, -inf */

    ray_t* a = ray_vec_from_raw(RAY_I32, i32v, 3);
    ray_t* b = ray_vec_from_raw(RAY_I16, i16v, 3);
    ray_t* c = ray_vec_from_raw(RAY_U8,  u8v,  3);
    ray_t* d = ray_vec_from_raw(RAY_F64, fv,   3);

    int64_t na = ray_sym_intern("a", 1);
    int64_t nb = ray_sym_intern("b", 1);
    int64_t nc = ray_sym_intern("c", 1);
    int64_t nd = ray_sym_intern("d", 1);

    ray_t* tbl = ray_table_new(4);
    tbl = ray_table_add_col(tbl, na, a);
    tbl = ray_table_add_col(tbl, nb, b);
    tbl = ray_table_add_col(tbl, nc, c);
    tbl = ray_table_add_col(tbl, nd, d);
    ray_release(a); ray_release(b); ray_release(c); ray_release(d);

    ray_err_t werr = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(werr, RAY_OK);

    /* Re-read; explicit F64 schema ensures the nan/inf strings parse. */
    int8_t schema[4] = { RAY_I64, RAY_I64, RAY_I64, RAY_F64 };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, ',', true, schema, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Write-side null cells -> empty fields (csv_write_cell early return). */
static test_result_t test_csv_write_null_cells(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t vals[3] = { 10, 0, 30 };
    ray_t* vec = ray_vec_from_raw(RAY_I64, vals, 3);
    /* Mark middle cell null. */
    ray_vec_set_null(vec, 1, true);

    int64_t nm = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Read it back, verify nullness preserved. */
    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(col, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 2));

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Slice column on the write side: csv_col_info_init exercises the
 * slice branch (data_owner = parent, base_row = offset). */
static test_result_t test_csv_write_sliced_column(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t vals[5] = { 100, 200, 300, 400, 500 };
    ray_t* parent = ray_vec_from_raw(RAY_I64, vals, 5);
    ray_t* sl = ray_vec_slice(parent, 1, 3);   /* 200, 300, 400 */

    int64_t nm = ray_sym_intern("x", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, sl);
    ray_release(sl); ray_release(parent);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    int64_t* d = (int64_t*)ray_data(ray_table_get_col_idx(loaded, 0));
    TEST_ASSERT_EQ_I(d[0], 200);
    TEST_ASSERT_EQ_I(d[1], 300);
    TEST_ASSERT_EQ_I(d[2], 400);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Header field whose name itself needs quoting (contains a comma).
 * Exercises csv_write_str's quote/escape branch on the header row. */
static test_result_t test_csv_header_needs_quoting(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a column whose intern'd name contains a comma + quote. */
    int64_t v[2] = { 1, 2 };
    ray_t* vec = ray_vec_from_raw(RAY_I64, v, 2);
    int64_t nm = ray_sym_intern("a,\"b", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, nm, vec);
    ray_release(vec);

    ray_err_t err = ray_write_csv(tbl, TMP_CSV);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Round-trip: header name is treated as a sym; the parser will
     * unescape the quoted header field. We just assert the file
     * loads back with two rows. */
    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 2);

    ray_release(loaded);
    ray_release(tbl);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Force the parallel parse path: > 8192 rows triggers ray_pool_dispatch.
 * This covers csv_parse_fn (vs. the serial fallback already exercised). */
static test_result_t test_csv_parallel_parse(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "i,s\n");
    /* 9000 rows so n_rows > 8192. */
    for (int i = 0; i < 9000; i++)
        fprintf(f, "%d,row%d\n", i, i);
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 9000);
    ray_t* ic = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(ic->type, RAY_I64);
    int64_t* id = (int64_t*)ray_data(ic);
    TEST_ASSERT_EQ_I(id[0], 0);
    TEST_ASSERT_EQ_I(id[8999], 8999);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Symbol-column narrowing: a small distinct-value count should narrow
 * the underlying vector to RAY_SYM_W8 (uint8_t indices). */
static test_result_t test_csv_sym_narrowing(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "k\n");
    /* Only three distinct values across many rows. */
    for (int i = 0; i < 200; i++)
        fprintf(f, "%s\n", (i % 3 == 0) ? "alpha" : (i % 3 == 1) ? "beta" : "gamma");
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_SYM);
    /* Width is encoded in the lower 2 bits of attrs (RAY_SYM_W8 == 0). */
    TEST_ASSERT_EQ_I((int)(col->attrs & RAY_SYM_W_MASK), RAY_SYM_W8);
    TEST_ASSERT_FALSE(col->attrs & RAY_ATTR_HAS_NULLS);
    /* Just sanity: rows exist and aren't null. */
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 200);
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 0));
    TEST_ASSERT_FALSE(ray_vec_is_null(col, 199));

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Explicit narrow-int schema: regression for the parse_types map that
 * silently routed RAY_U8/RAY_I16/RAY_I32 to CSV_TYPE_STR.  In that
 * state csv_intern_strings would write 4-byte sym IDs into the smaller
 * column buffer, overflowing the heap for U8/I16 (eventual SIGSEGV in
 * later allocators) and producing sym IDs instead of integers for I32. */

static test_result_t test_csv_explicit_u8_schema(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "v\n");
    /* 10 000 rows ⇒ parallel parse path; values 0..255 cycling so the
     * truncated bytes fully exercise the U8 range. */
    const int N = 10000;
    for (int i = 0; i < N; i++) fprintf(f, "%d\n", (i + 1) % 256);
    fclose(f);

    int8_t schema[1] = { RAY_U8 };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), N);

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_U8);
    const uint8_t* d = (const uint8_t*)ray_data(col);
    TEST_ASSERT_EQ_I((int)d[0], 1);
    TEST_ASSERT_EQ_I((int)d[N - 1], (int)((N) % 256));
    /* Spot-check a mid value to make sure we didn't get sym IDs. */
    TEST_ASSERT_EQ_I((int)d[100], (int)(101 % 256));
    /* No null sentinels expected; column must not advertise nulls. */
    TEST_ASSERT_FALSE(col->attrs & RAY_ATTR_HAS_NULLS);

    /* Sort it — without the fix this segfaults inside packed_radix_sort_run
     * on the corrupted heap. */
    ray_t* col_arg = col;
    uint8_t desc = 0, nf = 1;
    ray_t* idx = ray_sort_indices(&col_arg, &desc, &nf, 1, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idx));
    ray_release(idx);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_explicit_i16_schema_with_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "v\n");
    const int N = 1500;
    for (int i = 0; i < N; i++) {
        if (i % 200 == 0) fprintf(f, "\n");          /* empty → null */
        else fprintf(f, "%d\n", (i % 17) - 8);       /* range -8..8 */
    }
    fclose(f);

    int8_t schema[1] = { RAY_I16 };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), N);

    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_I16);
    TEST_ASSERT_TRUE(col->attrs & RAY_ATTR_HAS_NULLS);

    /* Every 200th row must be null, others must hold the parsed integer. */
    const int16_t* d = (const int16_t*)ray_data(col);
    int null_count = 0;
    for (int i = 0; i < N; i++) {
        if (i % 200 == 0) {
            TEST_ASSERT_TRUE(ray_vec_is_null(col, i));
            null_count++;
        } else {
            TEST_ASSERT_FALSE(ray_vec_is_null(col, i));
            TEST_ASSERT_EQ_I((int)d[i], (i % 17) - 8);
        }
    }
    TEST_ASSERT_EQ_I(null_count, (N + 199) / 200);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_explicit_i32_schema(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "v\n");
    const int N = 500;
    for (int i = 0; i < N; i++) fprintf(f, "%d\n", -100000 + i * 137);
    fclose(f);

    int8_t schema[1] = { RAY_I32 };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_EQ_I(col->type, RAY_I32);

    /* Without the fix this column would hold sym IDs (1, 2, 3, …) instead
     * of the actual integers. */
    const int32_t* d = (const int32_t*)ray_data(col);
    for (int i = 0; i < N; i++)
        TEST_ASSERT_EQ_I((int)d[i], -100000 + i * 137);

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_explicit_u8_schema_serial(void) {
    /* Force the serial parse path (n_rows ≤ 8192) and exercise truncated-row
     * fill defaults plus null-column path. */
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "a,b\n");
    /* 200 rows; second column missing on every 50th row → triggers
     * past-row-boundary fill in the parser. */
    for (int i = 0; i < 200; i++) {
        if (i % 50 == 0) fprintf(f, "%d\n", i);
        else fprintf(f, "%d,%d\n", i, i + 1);
    }
    fclose(f);

    int8_t schema[2] = { RAY_U8, RAY_U8 };
    ray_t* loaded = ray_read_csv_opts(TMP_CSV, 0, true, schema, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 200);

    ray_t* a = ray_table_get_col_idx(loaded, 0);
    ray_t* b = ray_table_get_col_idx(loaded, 1);
    TEST_ASSERT_EQ_I(a->type, RAY_U8);
    TEST_ASSERT_EQ_I(b->type, RAY_U8);
    TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_NULLS);

    const uint8_t* ad = (const uint8_t*)ray_data(a);
    const uint8_t* bd = (const uint8_t*)ray_data(b);
    for (int i = 0; i < 200; i++) {
        TEST_ASSERT_EQ_I((int)ad[i], i % 256);
        if (i % 50 == 0) TEST_ASSERT_TRUE(ray_vec_is_null(b, i));
        else {
            TEST_ASSERT_FALSE(ray_vec_is_null(b, i));
            TEST_ASSERT_EQ_I((int)bd[i], (i + 1) % 256);
        }
    }

    ray_release(loaded);
    unlink(TMP_CSV);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_csv_infer_high_cardinality_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    FILE* f = fopen(TMP_CSV, "w");
    fprintf(f, "payload\n");
    for (int i = 0; i < 100; i++)
        fprintf(f, "unique_payload_%03d\n", i);
    fclose(f);

    ray_t* loaded = ray_read_csv(TMP_CSV);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* col = ray_table_get_col_idx(loaded, 0);
    TEST_ASSERT_NOT_NULL(col);
    TEST_ASSERT_EQ_I(col->type, RAY_STR);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 100);

    ray_release(loaded);
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
    { "csv/infer_date", test_csv_infer_date, NULL, NULL },
    { "csv/infer_time", test_csv_infer_time, NULL, NULL },
    { "csv/infer_timestamp_promotion", test_csv_infer_timestamp_promotion, NULL, NULL },
    { "csv/infer_bool", test_csv_infer_bool, NULL, NULL },
    { "csv/infer_f64_specials", test_csv_infer_f64_specials, NULL, NULL },
    { "csv/infer_null_sentinels", test_csv_infer_null_sentinels, NULL, NULL },
    { "csv/infer_promotions", test_csv_infer_promotions, NULL, NULL },
    { "csv/tab_delimiter", test_csv_tab_delimiter, NULL, NULL },
    { "csv/no_header", test_csv_no_header, NULL, NULL },
    { "csv/read_missing_file", test_csv_read_missing_file, NULL, NULL },
    { "csv/write_bad_path", test_csv_write_bad_path, NULL, NULL },
    { "csv/invalid_schema_type", test_csv_invalid_schema_type, NULL, NULL },
    { "csv/crlf_line_endings", test_csv_crlf_line_endings, NULL, NULL },
    { "csv/truncated_row", test_csv_truncated_row, NULL, NULL },
    { "csv/roundtrip_date_time_ts", test_csv_roundtrip_date_time_ts, NULL, NULL },
    { "csv/write_int_widths_and_floats", test_csv_write_int_widths_and_floats, NULL, NULL },
    { "csv/write_null_cells", test_csv_write_null_cells, NULL, NULL },
    { "csv/write_sliced_column", test_csv_write_sliced_column, NULL, NULL },
    { "csv/header_needs_quoting", test_csv_header_needs_quoting, NULL, NULL },
    { "csv/parallel_parse", test_csv_parallel_parse, NULL, NULL },
    { "csv/sym_narrowing", test_csv_sym_narrowing, NULL, NULL },
    { "csv/infer_high_cardinality_str", test_csv_infer_high_cardinality_str, NULL, NULL },
    /* Narrow-int explicit schema (regression for missing parse_types map
     * entries that routed U8/I16/I32 to STR and corrupted the heap). */
    { "csv/explicit_u8_schema",  test_csv_explicit_u8_schema,             NULL, NULL },
    { "csv/explicit_i16_schema_with_nulls",
                                  test_csv_explicit_i16_schema_with_nulls, NULL, NULL },
    { "csv/explicit_i32_schema", test_csv_explicit_i32_schema,            NULL, NULL },
    { "csv/explicit_u8_schema_serial",
                                  test_csv_explicit_u8_schema_serial,      NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
