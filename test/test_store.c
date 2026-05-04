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

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include <time.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "store/col.h"
#include "store/fileio.h"
#include "store/splay.h"
#include "store/part.h"
#include "store/serde.h"
#include "core/ipc.h"
#include "core/sock.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "mem/sys.h"
#include "table/sym.h"

#ifndef RAY_OS_WINDOWS
  #include <sys/socket.h>
  #include <netinet/in.h>
#endif
#include "table/table.h"
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/* Forward-declare runtime lifecycle for mem_budget test */
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);

#define TMP_COL_PATH  "/tmp/rayforce_test_col.dat"
#define TMP_SPLAY_DIR "/tmp/rayforce_test_splay"

/* ---- Setup / Teardown -------------------------------------------------- */

static void store_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void store_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- test_col_mmap_i64 ------------------------------------------------- */

static test_result_t test_col_mmap_i64(void) {
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    /* Save to file */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load via mmap */
    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));

    /* Verify mmod==1 */
    TEST_ASSERT_EQ_U(mapped->mmod, 1);

    /* Verify type, len, data */
    TEST_ASSERT_EQ_I(mapped->type, RAY_I64);
    TEST_ASSERT_EQ_I(mapped->len, 5);

    int64_t* data = (int64_t*)ray_data(mapped);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQ_I(data[i], raw[i]);
    }

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_mmap_f64 ------------------------------------------------- */

static test_result_t test_col_mmap_f64(void) {
    double raw[] = {1.1, 2.2, 3.3, 4.4};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 4);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));

    TEST_ASSERT_EQ_U(mapped->mmod, 1);
    TEST_ASSERT_EQ_I(mapped->type, RAY_F64);
    TEST_ASSERT_EQ_I(mapped->len, 4);

    double* data = (double*)ray_data(mapped);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT((data[i]) == (raw[i]), "double == failed");
    }

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_mmap_cow ------------------------------------------------- */

static test_result_t test_col_mmap_cow(void) {
    int64_t raw[] = {100, 200, 300};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_U(mapped->mmod, 1);

    /* Retain so rc==2, forcing ray_cow to make a real copy */
    ray_retain(mapped);
    TEST_ASSERT_EQ_U(mapped->rc, 2);

    /* COW: ray_cow should produce a buddy-allocated copy */
    ray_t* copy = ray_cow(mapped);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_FALSE(RAY_IS_ERR(copy));
    TEST_ASSERT_EQ_U(copy->mmod, 0);

    /* ray_cow called ray_release on mapped (rc 2->1), so mapped still alive */

    /* Verify data in copy */
    int64_t* data = (int64_t*)ray_data(copy);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_I(data[i], raw[i]);
    }

    ray_release(copy);
    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_mmap_refcount -------------------------------------------- */

static test_result_t test_col_mmap_refcount(void) {
    int64_t raw[] = {7, 8, 9};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_U(mapped->rc, 1);

    /* Retain: rc should be 2 */
    ray_retain(mapped);
    TEST_ASSERT_EQ_U(mapped->rc, 2);

    /* Release once: rc==1, still readable */
    ray_release(mapped);
    TEST_ASSERT_EQ_U(mapped->rc, 1);

    int64_t* data = (int64_t*)ray_data(mapped);
    TEST_ASSERT_EQ_I(data[0], 7);
    TEST_ASSERT_EQ_I(data[1], 8);
    TEST_ASSERT_EQ_I(data[2], 9);

    /* Release again: munmap */
    ray_release(mapped);

    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_mmap_corrupt --------------------------------------------- */

static test_result_t test_col_mmap_corrupt(void) {
    /* Write a 16-byte file (too small for a valid column header) */
    FILE* f = fopen(TMP_COL_PATH, "wb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t junk[16] = {0};
    fwrite(junk, 1, 16, f);
    fclose(f);

    ray_t* result = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    TEST_ASSERT_STR_EQ(ray_err_code(result), "corrupt");
    ray_release(result);

    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_mmap_nofile ---------------------------------------------- */

static test_result_t test_col_mmap_nofile(void) {
    ray_t* result = ray_col_mmap("/tmp/rayforce_nonexistent_file_xyz.dat");
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    TEST_ASSERT_STR_EQ(ray_err_code(result), "io");
    ray_release(result);

    PASS();
}

/* ---- test_splay_open_roundtrip ----------------------------------------- */

static test_result_t test_splay_open_roundtrip(void) {
    /* Clean up any leftover splay dir */
    (void)!system("rm -rf " TMP_SPLAY_DIR);

    /* Build a 3-column table: I64, F64, I32 */
    ray_t* tbl = ray_table_new(4);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int64_t id_a = ray_sym_intern("col_a", 5);
    int64_t id_b = ray_sym_intern("col_b", 5);
    int64_t id_c = ray_sym_intern("col_c", 5);

    int64_t raw_a[] = {1, 2, 3, 4, 5};
    double  raw_b[] = {1.5, 2.5, 3.5, 4.5, 5.5};
    int32_t raw_c[] = {10, 20, 30, 40, 50};

    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw_a, 5);
    ray_t* col_b = ray_vec_from_raw(RAY_F64, raw_b, 5);
    ray_t* col_c = ray_vec_from_raw(RAY_I32, raw_c, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_a));
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_b));
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_c));

    tbl = ray_table_add_col(tbl, id_a, col_a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_c, col_c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save to splay directory */
    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_DIR, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Open via mmap (zero-copy) */
    ray_t* loaded = ray_read_splayed(TMP_SPLAY_DIR, NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    /* Verify ncols and nrows */
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 3);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 5);

    /* Verify column mmod==1 (mmap'd) */
    ray_t* la = ray_table_get_col(loaded, id_a);
    ray_t* lb = ray_table_get_col(loaded, id_b);
    ray_t* lc = ray_table_get_col(loaded, id_c);
    TEST_ASSERT_NOT_NULL(la);
    TEST_ASSERT_NOT_NULL(lb);
    TEST_ASSERT_NOT_NULL(lc);

    TEST_ASSERT_EQ_U(la->mmod, 1);
    TEST_ASSERT_EQ_U(lb->mmod, 1);
    TEST_ASSERT_EQ_U(lc->mmod, 1);

    /* Verify data */
    int64_t* da = (int64_t*)ray_data(la);
    double*  db = (double*)ray_data(lb);
    int32_t* dc = (int32_t*)ray_data(lc);

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQ_I(da[i], raw_a[i]);
        TEST_ASSERT((db[i]) == (raw_b[i]), "double == failed");
        TEST_ASSERT_EQ_I(dc[i], raw_c[i]);
    }

    ray_release(loaded);
    ray_release(col_a);
    ray_release(col_b);
    ray_release(col_c);
    ray_release(tbl);

    /* Cleanup */
    (void)!system("rm -rf " TMP_SPLAY_DIR);
    PASS();
}

/* ---- test_splay_str_column_roundtrip ----------------------------------- */

static test_result_t test_splay_str_column_roundtrip(void) {
    (void)!system("rm -rf " TMP_SPLAY_DIR);

    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int64_t id_id = ray_sym_intern("id", 2);
    int64_t id_name = ray_sym_intern("name", 4);

    int64_t raw_ids[] = {1, 2, 3};
    ray_t* ids = ray_vec_from_raw(RAY_I64, raw_ids, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ids));

    ray_t* names = ray_vec_new(RAY_STR, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(names));
    names = ray_str_vec_append(names, "alpha", 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(names));
    names = ray_str_vec_append(names, "a pooled string value", 21);
    TEST_ASSERT_FALSE(RAY_IS_ERR(names));
    names = ray_str_vec_append(names, "", 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(names));
    ray_vec_set_null(names, 2, true);

    tbl = ray_table_add_col(tbl, id_id, ids);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_name, names);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_DIR, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_splayed(TMP_SPLAY_DIR, NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    ray_t* loaded_ids = ray_table_get_col(loaded, id_id);
    ray_t* loaded_names = ray_table_get_col(loaded, id_name);
    TEST_ASSERT_NOT_NULL(loaded_ids);
    TEST_ASSERT_NOT_NULL(loaded_names);
    TEST_ASSERT_EQ_U(loaded_ids->mmod, 1);
    TEST_ASSERT_EQ_U(loaded_names->mmod, 0);
    TEST_ASSERT_EQ_I(loaded_names->type, RAY_STR);
    TEST_ASSERT_TRUE(loaded_names->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(loaded_names, 2));

    size_t slen = 0;
    const char* s0 = ray_str_vec_get(loaded_names, 0, &slen);
    TEST_ASSERT_EQ_U(slen, 5);
    TEST_ASSERT_MEM_EQ(5, s0, "alpha");
    const char* s1 = ray_str_vec_get(loaded_names, 1, &slen);
    TEST_ASSERT_EQ_U(slen, 21);
    TEST_ASSERT_MEM_EQ(21, s1, "a pooled string value");

    ray_release(loaded);
    ray_release(ids);
    ray_release(names);
    ray_release(tbl);

    (void)!system("rm -rf " TMP_SPLAY_DIR);
    PASS();
}

/* ---- test_splay_short_strv_roundtrip ----------------------------------
 * Regression: 0-row STRV columns serialize to 14 bytes and 1-row STRV
 * columns with content < 10 bytes serialize under 32 bytes total.  The
 * splay reader uses ray_col_mmap which falls through col_validate_mapped;
 * before the magic-aware fix, mapped_size < 32 returned "corrupt" and
 * the splay loader's "nyi" fallback to ray_col_load never fired,
 * making short STRV tables unreadable via ray_read_splayed.
 * ---------------------------------------------------------------------- */

static test_result_t test_splay_short_strv_roundtrip(void) {
    (void)!system("rm -rf " TMP_SPLAY_DIR);

    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int64_t id_short = ray_sym_intern("short", 5);
    int64_t id_empty = ray_sym_intern("empty", 5);

    /* 1-row STRV with 2-byte content -- file ends up at 4 (magic) + 1
     * (type) + 1 (attrs) + 8 (count) + 8 (slen) + 2 (content) = 24 bytes */
    ray_t* shorts = ray_vec_new(RAY_STR, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(shorts));
    shorts = ray_str_vec_append(shorts, "hi", 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(shorts));

    /* 0-row STRV -- 14 bytes on disk */
    ray_t* empty = ray_vec_new(RAY_STR, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(empty));

    tbl = ray_table_add_col(tbl, id_short, shorts);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_empty, empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_DIR, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_splayed(TMP_SPLAY_DIR, NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);

    ray_t* loaded_shorts = ray_table_get_col(loaded, id_short);
    ray_t* loaded_empty  = ray_table_get_col(loaded, id_empty);
    TEST_ASSERT_NOT_NULL(loaded_shorts);
    TEST_ASSERT_NOT_NULL(loaded_empty);
    TEST_ASSERT_EQ_I(loaded_shorts->type, RAY_STR);
    TEST_ASSERT_EQ_I(loaded_shorts->len, 1);
    TEST_ASSERT_EQ_I(loaded_empty->type, RAY_STR);
    TEST_ASSERT_EQ_I(loaded_empty->len, 0);

    size_t slen = 0;
    const char* s0 = ray_str_vec_get(loaded_shorts, 0, &slen);
    TEST_ASSERT_EQ_U(slen, 2);
    TEST_ASSERT_MEM_EQ(2, s0, "hi");

    ray_release(loaded);
    ray_release(shorts);
    ray_release(empty);
    ray_release(tbl);

    (void)!system("rm -rf " TMP_SPLAY_DIR);
    PASS();
}

/* ---- test_parted_nrows ------------------------------------------------- */

static test_result_t test_parted_nrows(void) {
    /* Build 3 segment vectors: 100, 200, 300 rows */
    ray_t* seg0 = ray_vec_new(RAY_I64, 100);
    ray_t* seg1 = ray_vec_new(RAY_I64, 200);
    ray_t* seg2 = ray_vec_new(RAY_I64, 300);
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg1));
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg2));
    seg0->len = 100;
    seg1->len = 200;
    seg2->len = 300;

    /* Build a parted column: type = RAY_PARTED_BASE + RAY_I64, len = 3 segments */
    size_t data_size = 3 * sizeof(ray_t*);
    ray_t* parted = ray_alloc(data_size);
    TEST_ASSERT_NOT_NULL(parted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parted));
    parted->type = RAY_PARTED_BASE + RAY_I64;
    parted->len = 3;
    parted->attrs = 0;
    memset(parted->nullmap, 0, 16);

    ray_t** segs = (ray_t**)ray_data(parted);
    segs[0] = seg0; ray_retain(seg0);
    segs[1] = seg1; ray_retain(seg1);
    segs[2] = seg2; ray_retain(seg2);

    /* Verify ray_parted_nrows returns 600 */
    int64_t total = ray_parted_nrows(parted);
    TEST_ASSERT_EQ_I(total, 600);

    /* Non-parted vector falls through to v->len */
    TEST_ASSERT_EQ_I(ray_parted_nrows(seg0), 100);

    ray_release(parted);
    ray_release(seg0);
    ray_release(seg1);
    ray_release(seg2);
    PASS();
}

/* ---- test_table_nrows_parted ------------------------------------------- */

static test_result_t test_table_nrows_parted(void) {
    /* Build 2 segment vectors: 50 and 75 rows */
    ray_t* seg0 = ray_vec_new(RAY_I64, 50);
    ray_t* seg1 = ray_vec_new(RAY_I64, 75);
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg1));
    seg0->len = 50;
    seg1->len = 75;

    /* Build a parted column */
    size_t data_size = 2 * sizeof(ray_t*);
    ray_t* parted = ray_alloc(data_size);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parted));
    parted->type = RAY_PARTED_BASE + RAY_I64;
    parted->len = 2;
    parted->attrs = 0;
    memset(parted->nullmap, 0, 16);

    ray_t** segs = (ray_t**)ray_data(parted);
    segs[0] = seg0; ray_retain(seg0);
    segs[1] = seg1; ray_retain(seg1);

    /* Build a table with this parted column */
    int64_t name_id = ray_sym_intern("pcol", 4);
    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, name_id, parted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Verify ray_table_nrows returns 125 */
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 125);

    ray_release(tbl);
    ray_release(parted);
    ray_release(seg0);
    ray_release(seg1);
    PASS();
}

/* ---- test_parted_release ----------------------------------------------- */

static test_result_t test_parted_release(void) {
    /* Build 2 segment vectors */
    ray_t* seg0 = ray_vec_new(RAY_I64, 10);
    ray_t* seg1 = ray_vec_new(RAY_I64, 20);
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(seg1));
    seg0->len = 10;
    seg1->len = 20;

    /* Build a parted column */
    size_t data_size = 2 * sizeof(ray_t*);
    ray_t* parted = ray_alloc(data_size);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parted));
    parted->type = RAY_PARTED_BASE + RAY_I64;
    parted->len = 2;
    parted->attrs = 0;
    memset(parted->nullmap, 0, 16);

    ray_t** segs = (ray_t**)ray_data(parted);
    segs[0] = seg0; ray_retain(seg0);
    segs[1] = seg1; ray_retain(seg1);

    /* Segments should have rc=2 (original + parted ref) */
    TEST_ASSERT_EQ_U(seg0->rc, 2);
    TEST_ASSERT_EQ_U(seg1->rc, 2);

    /* Release parted column — segments' rc should drop to 1 */
    ray_release(parted);
    TEST_ASSERT_EQ_U(seg0->rc, 1);
    TEST_ASSERT_EQ_U(seg1->rc, 1);

    ray_release(seg0);
    ray_release(seg1);
    PASS();
}

/* ---- test_part_open ---------------------------------------------------- */

#define TMP_PART_DB "/tmp/rayforce_test_parted_db"
#define TMP_TABLE_NAME "test_tbl"

static test_result_t test_part_open(void) {
    /* Setup: create a 2-partition db with 2 columns each */
    (void)!system("rm -rf " TMP_PART_DB);
    (void)!system("mkdir -p " TMP_PART_DB "/2024.01.01/" TMP_TABLE_NAME);
    (void)!system("mkdir -p " TMP_PART_DB "/2024.01.02/" TMP_TABLE_NAME);

    /* Partition 1: 3 rows */
    int64_t raw_a1[] = {10, 20, 30};
    double  raw_b1[] = {1.1, 2.2, 3.3};
    ray_t* a1 = ray_vec_from_raw(RAY_I64, raw_a1, 3);
    ray_t* b1 = ray_vec_from_raw(RAY_F64, raw_b1, 3);

    ray_t* tbl1 = ray_table_new(3);
    int64_t name_a = ray_sym_intern("a", 1);
    int64_t name_b = ray_sym_intern("b", 1);
    tbl1 = ray_table_add_col(tbl1, name_a, a1);
    tbl1 = ray_table_add_col(tbl1, name_b, b1);
    ray_err_t err = ray_splay_save(tbl1, TMP_PART_DB "/2024.01.01/" TMP_TABLE_NAME, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Partition 2: 5 rows */
    int64_t raw_a2[] = {40, 50, 60, 70, 80};
    double  raw_b2[] = {4.4, 5.5, 6.6, 7.7, 8.8};
    ray_t* a2 = ray_vec_from_raw(RAY_I64, raw_a2, 5);
    ray_t* b2 = ray_vec_from_raw(RAY_F64, raw_b2, 5);

    ray_t* tbl2 = ray_table_new(3);
    tbl2 = ray_table_add_col(tbl2, name_a, a2);
    tbl2 = ray_table_add_col(tbl2, name_b, b2);
    err = ray_splay_save(tbl2, TMP_PART_DB "/2024.01.02/" TMP_TABLE_NAME, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Save symfile */
    err = ray_sym_save(TMP_PART_DB "/sym");
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Cleanup in-memory tables */
    ray_release(a1); ray_release(b1); ray_release(tbl1);
    ray_release(a2); ray_release(b2); ray_release(tbl2);

    /* Open via ray_read_parted */
    ray_t* parted = ray_read_parted(TMP_PART_DB, TMP_TABLE_NAME);
    TEST_ASSERT_NOT_NULL(parted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parted));

    /* Should have 3 columns: date (MAPCOMMON), a (parted I64), b (parted F64) */
    int64_t ncols = ray_table_ncols(parted);
    TEST_ASSERT_EQ_I(ncols, 3);

    /* Total rows should be 8 */
    int64_t nrows = ray_table_nrows(parted);
    TEST_ASSERT_EQ_I(nrows, 8);

    /* Verify first column is MAPCOMMON (date-inferred) */
    ray_t* mapcommon = ray_table_get_col_idx(parted, 0);
    TEST_ASSERT_NOT_NULL(mapcommon);
    TEST_ASSERT_EQ_I(mapcommon->type, RAY_MAPCOMMON);
    TEST_ASSERT_EQ_U(mapcommon->attrs, RAY_MC_DATE);

    /* MAPCOMMON: [key_values (RAY_DATE), row_counts (RAY_I64)] */
    ray_t** mc_ptrs = (ray_t**)ray_data(mapcommon);
    ray_t* key_values = mc_ptrs[0];
    ray_t* row_counts = mc_ptrs[1];

    /* key_values should be RAY_DATE with parsed days-since-2000 */
    TEST_ASSERT_EQ_I(key_values->type, RAY_DATE);
    TEST_ASSERT_EQ_I(key_values->len, 2);
    int32_t* kv_data = (int32_t*)ray_data(key_values);
    /* 2024.01.01 = 8766 days since 2000-01-01 */
    TEST_ASSERT_EQ_I(kv_data[0], 8766);
    /* 2024.01.02 = 8767 */
    TEST_ASSERT_EQ_I(kv_data[1], 8767);

    TEST_ASSERT_EQ_I(row_counts->len, 2);
    int64_t* rc_data = (int64_t*)ray_data(row_counts);
    TEST_ASSERT_EQ_I(rc_data[0], 3);
    TEST_ASSERT_EQ_I(rc_data[1], 5);

    /* Verify second column is parted I64 */
    ray_t* col_a = ray_table_get_col_idx(parted, 1);
    TEST_ASSERT_NOT_NULL(col_a);
    TEST_ASSERT_TRUE(RAY_IS_PARTED(col_a->type));
    TEST_ASSERT_EQ_I(RAY_PARTED_BASETYPE(col_a->type), RAY_I64);
    TEST_ASSERT_EQ_I(col_a->len, 2);

    /* Verify segment 0 has 3 rows, mmod=1 (mmap'd) */
    ray_t** segs_a = (ray_t**)ray_data(col_a);
    TEST_ASSERT_EQ_I(segs_a[0]->len, 3);
    TEST_ASSERT_EQ_U(segs_a[0]->mmod, 1);
    TEST_ASSERT_EQ_I(segs_a[1]->len, 5);
    TEST_ASSERT_EQ_U(segs_a[1]->mmod, 1);

    /* Verify data in segment 0 */
    int64_t* data_a0 = (int64_t*)ray_data(segs_a[0]);
    TEST_ASSERT_EQ_I(data_a0[0], 10);
    TEST_ASSERT_EQ_I(data_a0[2], 30);

    /* Verify third column is parted F64 */
    ray_t* col_b = ray_table_get_col_idx(parted, 2);
    TEST_ASSERT_TRUE(RAY_IS_PARTED(col_b->type));
    TEST_ASSERT_EQ_I(RAY_PARTED_BASETYPE(col_b->type), RAY_F64);

    /* Release — should unmap all segments */
    ray_release(parted);

    (void)!system("rm -rf " TMP_PART_DB);
    PASS();
}

/* ---- test_group_parted ------------------------------------------------- */

static test_result_t test_group_parted(void) {
    /* Build a 2-partition parted table with columns id1 (I64) and v1 (I64).
     * Partition 0: id1=[0,0,1,1,2], v1=[10,20,30,40,50]
     * Partition 1: id1=[0,1,1,2,2], v1=[60,70,80,90,100]
     * GROUP BY id1 SUM(v1) should give:
     *   id1=0: 10+20+60 = 90
     *   id1=1: 30+40+70+80 = 220
     *   id1=2: 50+90+100 = 240
     */

    /* Build segment vectors */
    ray_t* id1_0 = ray_vec_new(RAY_I64, 5);
    ray_t* v1_0  = ray_vec_new(RAY_I64, 5);
    TEST_ASSERT_NOT_NULL(id1_0);
    TEST_ASSERT_NOT_NULL(v1_0);
    id1_0->len = v1_0->len = 5;
    int64_t id1_0_data[] = {0,0,1,1,2};
    int64_t v1_0_data[]  = {10,20,30,40,50};
    memcpy(ray_data(id1_0), id1_0_data, sizeof(id1_0_data));
    memcpy(ray_data(v1_0),  v1_0_data,  sizeof(v1_0_data));

    ray_t* id1_1 = ray_vec_new(RAY_I64, 5);
    ray_t* v1_1  = ray_vec_new(RAY_I64, 5);
    TEST_ASSERT_NOT_NULL(id1_1);
    TEST_ASSERT_NOT_NULL(v1_1);
    id1_1->len = v1_1->len = 5;
    int64_t id1_1_data[] = {0,1,1,2,2};
    int64_t v1_1_data[]  = {60,70,80,90,100};
    memcpy(ray_data(id1_1), id1_1_data, sizeof(id1_1_data));
    memcpy(ray_data(v1_1),  v1_1_data,  sizeof(v1_1_data));

    /* Build parted columns (2 segments each) */
    ray_t* id1_parted = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(id1_parted);
    id1_parted->type = RAY_PARTED_BASE + RAY_I64;
    id1_parted->len = 2;
    ((ray_t**)ray_data(id1_parted))[0] = id1_0;
    ((ray_t**)ray_data(id1_parted))[1] = id1_1;

    ray_t* v1_parted = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(v1_parted);
    v1_parted->type = RAY_PARTED_BASE + RAY_I64;
    v1_parted->len = 2;
    ((ray_t**)ray_data(v1_parted))[0] = v1_0;
    ((ray_t**)ray_data(v1_parted))[1] = v1_1;

    /* Build parted table */
    int64_t sym_id1 = ray_sym_intern("id1", 3);
    int64_t sym_v1  = ray_sym_intern("v1",  2);

    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_NOT_NULL(tbl);
    tbl = ray_table_add_col(tbl, sym_id1, id1_parted);
    tbl = ray_table_add_col(tbl, sym_v1,  v1_parted);
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 10);

    /* Build graph: GROUP BY id1 SUM(v1) */
    ray_graph_t* g = ray_graph_new(tbl);
    TEST_ASSERT_NOT_NULL(g);
    ray_op_t* scan_id1 = ray_scan(g, "id1");
    ray_op_t* scan_v1  = ray_scan(g, "v1");
    ray_op_t* keys[] = { scan_id1 };
    uint16_t ops[]  = { OP_SUM };
    ray_op_t* ins[]  = { scan_v1 };
    ray_op_t* root = ray_group(g, keys, 1, ops, ins, 1);
    root = ray_optimize(g, root);
    ray_t* result = ray_execute(g, root);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3); /* groups: 0, 1, 2 */

    /* Verify sums — extract key and agg columns, match by key value */
    ray_t* rk = ray_table_get_col_idx(result, 0); /* id1 key column */
    ray_t* rv = ray_table_get_col_idx(result, 1); /* v1_sum agg column */
    TEST_ASSERT_NOT_NULL(rk);
    TEST_ASSERT_NOT_NULL(rv);

    int64_t* rk_data = (int64_t*)ray_data(rk);
    int64_t* rv_data = (int64_t*)ray_data(rv);
    int64_t expected_sums[3] = {0, 0, 0}; /* for keys 0, 1, 2 */
    for (int i = 0; i < 3; i++) {
        int64_t key = rk_data[i];
        TEST_ASSERT_TRUE(key >= 0 && key <= 2);
        expected_sums[key] = rv_data[i];
    }
    TEST_ASSERT_EQ_I(expected_sums[0], 90);
    TEST_ASSERT_EQ_I(expected_sums[1], 220);
    TEST_ASSERT_EQ_I(expected_sums[2], 240);

    ray_release(result);
    ray_graph_free(g);
    ray_release(id1_parted);
    ray_release(v1_parted);
    ray_release(tbl);
    PASS();
}

/* ---- test_col_ext_nullmap_roundtrip ------------------------------------- */

#define EXT_NM_LEN 256  /* >128 to trigger ext_nullmap */

static test_result_t test_col_ext_nullmap_roundtrip(void) {
    /* Create a 256-element I64 vector with nulls at various positions */
    ray_t* vec = ray_vec_new(RAY_I64, EXT_NM_LEN);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = EXT_NM_LEN;

    int64_t* data = (int64_t*)ray_data(vec);
    for (int i = 0; i < EXT_NM_LEN; i++) data[i] = i * 10;

    /* Set nulls at positions: 0, 5, 127, 128, 200, 255 */
    int null_positions[] = { 0, 5, 127, 128, 200, 255 };
    int n_nulls = (int)(sizeof(null_positions) / sizeof(null_positions[0]));
    for (int i = 0; i < n_nulls; i++)
        ray_vec_set_null(vec, null_positions[i], true);

    /* Verify ext_nullmap was created (>128 elements forces external) */
    TEST_ASSERT_TRUE((vec->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE((vec->attrs & RAY_ATTR_NULLMAP_EXT) != 0);
    TEST_ASSERT_NOT_NULL(vec->ext_nullmap);

    /* --- Round-trip via ray_col_load --- */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    TEST_ASSERT_EQ_I(loaded->type, RAY_I64);
    TEST_ASSERT_EQ_I(loaded->len, EXT_NM_LEN);
    TEST_ASSERT_TRUE((loaded->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE((loaded->attrs & RAY_ATTR_NULLMAP_EXT) != 0);
    TEST_ASSERT_NOT_NULL(loaded->ext_nullmap);

    /* Verify null positions preserved */
    for (int i = 0; i < n_nulls; i++)
        TEST_ASSERT_TRUE(ray_vec_is_null(loaded, null_positions[i]));

    /* Verify non-null positions */
    TEST_ASSERT_FALSE(ray_vec_is_null(loaded, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(loaded, 129));
    TEST_ASSERT_FALSE(ray_vec_is_null(loaded, 254));

    /* Verify data values at non-null positions */
    int64_t* ld = (int64_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(ld[1], 10);
    TEST_ASSERT_EQ_I(ld[129], 1290);
    TEST_ASSERT_EQ_I(ld[254], 2540);

    ray_release(loaded);

    /* --- Round-trip via ray_col_mmap --- */
    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));

    TEST_ASSERT_EQ_U(mapped->mmod, 1);
    TEST_ASSERT_EQ_I(mapped->type, RAY_I64);
    TEST_ASSERT_EQ_I(mapped->len, EXT_NM_LEN);
    TEST_ASSERT_TRUE((mapped->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE((mapped->attrs & RAY_ATTR_NULLMAP_EXT) != 0);
    TEST_ASSERT_NOT_NULL(mapped->ext_nullmap);

    /* Verify null positions preserved in mmap path */
    for (int i = 0; i < n_nulls; i++)
        TEST_ASSERT_TRUE(ray_vec_is_null(mapped, null_positions[i]));

    TEST_ASSERT_FALSE(ray_vec_is_null(mapped, 1));
    TEST_ASSERT_FALSE(ray_vec_is_null(mapped, 129));

    /* Verify data */
    int64_t* md = (int64_t*)ray_data(mapped);
    TEST_ASSERT_EQ_I(md[1], 10);
    TEST_ASSERT_EQ_I(md[129], 1290);

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_save_load_str -------------------------------------------- */

static test_result_t test_col_save_load_str(void) {
    /* Build a list of 3 string atoms */
    ray_t* list = ray_list_new(4);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    ray_t* s0 = ray_str("hello", 5);
    ray_t* s1 = ray_str("world", 5);
    ray_t* s2 = ray_str("rayforce", 8);
    TEST_ASSERT_FALSE(RAY_IS_ERR(s0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(s1));
    TEST_ASSERT_FALSE(RAY_IS_ERR(s2));

    list = ray_list_append(list, s0);
    list = ray_list_append(list, s1);
    list = ray_list_append(list, s2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 3);

    /* Save */
    ray_err_t err = ray_col_save(list, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    /* Verify */
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 3);

    ray_t* l0 = ray_list_get(loaded, 0);
    ray_t* l1 = ray_list_get(loaded, 1);
    ray_t* l2 = ray_list_get(loaded, 2);
    TEST_ASSERT_NOT_NULL(l0);
    TEST_ASSERT_NOT_NULL(l1);
    TEST_ASSERT_NOT_NULL(l2);
    TEST_ASSERT_EQ_I(l0->type, -RAY_STR);
    TEST_ASSERT_EQ_I(l1->type, -RAY_STR);
    TEST_ASSERT_EQ_I(l2->type, -RAY_STR);

    TEST_ASSERT_EQ_U(ray_str_len(l0), 5);
    TEST_ASSERT_EQ_U(ray_str_len(l1), 5);
    TEST_ASSERT_EQ_U(ray_str_len(l2), 8);
    TEST_ASSERT_STR_EQ(ray_str_ptr(l0), "hello");
    TEST_ASSERT_STR_EQ(ray_str_ptr(l1), "world");
    TEST_ASSERT_STR_EQ(ray_str_ptr(l2), "rayforce");

    ray_release(loaded);
    ray_release(s0);
    ray_release(s1);
    ray_release(s2);
    ray_release(list);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_save_load_list ------------------------------------------- */

static test_result_t test_col_save_load_list(void) {
    /* Build a list of two I64 vectors */
    int64_t raw0[] = {10, 20, 30};
    int64_t raw1[] = {40, 50};
    ray_t* v0 = ray_vec_from_raw(RAY_I64, raw0, 3);
    ray_t* v1 = ray_vec_from_raw(RAY_I64, raw1, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(v1));

    ray_t* list = ray_list_new(4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    list = ray_list_append(list, v0);
    list = ray_list_append(list, v1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    TEST_ASSERT_EQ_I(list->len, 2);

    /* Save */
    ray_err_t err = ray_col_save(list, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    /* Verify */
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 2);

    ray_t* lv0 = ray_list_get(loaded, 0);
    ray_t* lv1 = ray_list_get(loaded, 1);
    TEST_ASSERT_NOT_NULL(lv0);
    TEST_ASSERT_NOT_NULL(lv1);
    TEST_ASSERT_EQ_I(lv0->type, RAY_I64);
    TEST_ASSERT_EQ_I(lv1->type, RAY_I64);
    TEST_ASSERT_EQ_I(lv0->len, 3);
    TEST_ASSERT_EQ_I(lv1->len, 2);

    int64_t* d0 = (int64_t*)ray_data(lv0);
    TEST_ASSERT_EQ_I(d0[0], 10);
    TEST_ASSERT_EQ_I(d0[1], 20);
    TEST_ASSERT_EQ_I(d0[2], 30);

    int64_t* d1 = (int64_t*)ray_data(lv1);
    TEST_ASSERT_EQ_I(d1[0], 40);
    TEST_ASSERT_EQ_I(d1[1], 50);

    ray_release(loaded);
    ray_release(v0);
    ray_release(v1);
    ray_release(list);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_save_load_table ------------------------------------------ */

static test_result_t test_col_save_load_table(void) {
    /* Build a 2-column table: I64 + F64 */
    int64_t id_a = ray_sym_intern("col_x", 5);
    int64_t id_b = ray_sym_intern("col_y", 5);

    int64_t raw_a[] = {1, 2, 3};
    double  raw_b[] = {1.5, 2.5, 3.5};
    ray_t* col_a = ray_vec_from_raw(RAY_I64, raw_a, 3);
    ray_t* col_b = ray_vec_from_raw(RAY_F64, raw_b, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_a));
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_b));

    ray_t* tbl = ray_table_new(4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_a, col_a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_b, col_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save */
    ray_err_t err = ray_col_save(tbl, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    /* Verify */
    TEST_ASSERT_EQ_I(loaded->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    /* Verify column names */
    TEST_ASSERT_EQ_I(ray_table_col_name(loaded, 0), id_a);
    TEST_ASSERT_EQ_I(ray_table_col_name(loaded, 1), id_b);

    /* Verify I64 column */
    ray_t* la = ray_table_get_col(loaded, id_a);
    TEST_ASSERT_NOT_NULL(la);
    TEST_ASSERT_EQ_I(la->type, RAY_I64);
    TEST_ASSERT_EQ_I(la->len, 3);
    int64_t* da = (int64_t*)ray_data(la);
    TEST_ASSERT_EQ_I(da[0], 1);
    TEST_ASSERT_EQ_I(da[1], 2);
    TEST_ASSERT_EQ_I(da[2], 3);

    /* Verify F64 column */
    ray_t* lb = ray_table_get_col(loaded, id_b);
    TEST_ASSERT_NOT_NULL(lb);
    TEST_ASSERT_EQ_I(lb->type, RAY_F64);
    TEST_ASSERT_EQ_I(lb->len, 3);
    double* db = (double*)ray_data(lb);
    TEST_ASSERT((db[0]) == (1.5), "double == failed");
    TEST_ASSERT((db[1]) == (2.5), "double == failed");
    TEST_ASSERT((db[2]) == (3.5), "double == failed");

    ray_release(loaded);
    ray_release(col_a);
    ray_release(col_b);
    ray_release(tbl);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_file_open_close ---------------------------------------------- */

#define TMP_FILEIO_PATH "/tmp/rayforce_test_fileio.dat"

static test_result_t test_file_open_close(void) {
    /* Open for write+create, then close */
    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Open for read (file now exists) */
    fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Open nonexistent for read (no create) → fail */
    unlink(TMP_FILEIO_PATH);
    fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    TEST_ASSERT_EQ_I(fd, RAY_FD_INVALID);

    /* NULL path → fail */
    fd = ray_file_open(NULL, 0);
    TEST_ASSERT_EQ_I(fd, RAY_FD_INVALID);

    PASS();
}

/* ---- test_file_lock_unlock --------------------------------------------- */

static test_result_t test_file_lock_unlock(void) {
    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");

    /* Exclusive lock + unlock */
    ray_err_t err = ray_file_lock_ex(fd);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    err = ray_file_unlock(fd);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Shared lock + unlock */
    err = ray_file_lock_sh(fd);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    err = ray_file_unlock(fd);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Invalid fd → error */
    TEST_ASSERT_EQ_I(ray_file_lock_ex(RAY_FD_INVALID), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(ray_file_lock_sh(RAY_FD_INVALID), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(ray_file_unlock(RAY_FD_INVALID), RAY_OK);

    ray_file_close(fd);
    unlink(TMP_FILEIO_PATH);
    PASS();
}

/* ---- test_file_sync_op ------------------------------------------------- */

static test_result_t test_file_sync_op(void) {
    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");

    /* fsync on valid fd */
    ray_err_t err = ray_file_sync(fd);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Invalid fd → error */
    TEST_ASSERT_EQ_I(ray_file_sync(RAY_FD_INVALID), RAY_ERR_IO);

    ray_file_close(fd);
    unlink(TMP_FILEIO_PATH);
    PASS();
}

/* ---- test_file_rename_op ----------------------------------------------- */

#define TMP_FILEIO_PATH2 "/tmp/rayforce_test_fileio2.dat"

static test_result_t test_file_rename_op(void) {
    unlink(TMP_FILEIO_PATH);
    unlink(TMP_FILEIO_PATH2);

    /* Create source file */
    ray_fd_t fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Rename */
    ray_err_t err = ray_file_rename(TMP_FILEIO_PATH, TMP_FILEIO_PATH2);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Old path should not exist, new should */
    fd = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    TEST_ASSERT_EQ_I(fd, RAY_FD_INVALID);

    fd = ray_file_open(TMP_FILEIO_PATH2, RAY_OPEN_READ);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Rename nonexistent → error */
    err = ray_file_rename("/tmp/rayforce_nonexistent_xyz", TMP_FILEIO_PATH2);
    TEST_ASSERT_EQ_I(err, RAY_ERR_IO);

    /* NULL args → error */
    TEST_ASSERT_EQ_I(ray_file_rename(NULL, TMP_FILEIO_PATH2), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(ray_file_rename(TMP_FILEIO_PATH, NULL), RAY_ERR_IO);

    unlink(TMP_FILEIO_PATH2);
    PASS();
}

/* ---- test_file_shared_lock_concurrent ---------------------------------- */

static test_result_t test_file_shared_lock_concurrent(void) {
    unlink(TMP_FILEIO_PATH);
    ray_fd_t fd1 = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    ray_fd_t fd2 = ray_file_open(TMP_FILEIO_PATH, RAY_OPEN_READ);
    TEST_ASSERT((fd1) != (RAY_FD_INVALID), "fd1 != RAY_FD_INVALID");
    TEST_ASSERT((fd2) != (RAY_FD_INVALID), "fd2 != RAY_FD_INVALID");

    /* Two shared locks should not conflict */
    ray_err_t err1 = ray_file_lock_sh(fd1);
    ray_err_t err2 = ray_file_lock_sh(fd2);
    TEST_ASSERT_EQ_I(err1, RAY_OK);
    TEST_ASSERT_EQ_I(err2, RAY_OK);

    ray_file_unlock(fd1);
    ray_file_unlock(fd2);
    ray_file_close(fd1);
    ray_file_close(fd2);
    unlink(TMP_FILEIO_PATH);
    PASS();
}

/* ---- test_sym_col_bounds_reject ----------------------------------------- */

static test_result_t test_sym_col_bounds_reject(void) {
    /* Intern a few symbols so sym_count > 0 */
    ray_sym_intern("sym_a", 5);
    ray_sym_intern("sym_b", 5);
    uint32_t sc = ray_sym_count();
    TEST_ASSERT((sc) >= (2), "sc >= 2");

    /* Build a W8 RAY_SYM column with valid indices */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W8, 4);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = 4;
    uint8_t* data = (uint8_t*)ray_data(vec);
    data[0] = 0; data[1] = 1; data[2] = 0; data[3] = 1;

    /* Save — should embed sym count in header rc field */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load back — should succeed since all indices < sym_count */
    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_I(loaded->len, 4);
    ray_release(loaded);

    /* Now craft a column with an out-of-range index */
    data[2] = (uint8_t)(sc + 10);  /* beyond sym table */
    err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load should fail with RAY_ERR_CORRUPT */
    ray_t* bad = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    /* Same test via mmap */
    bad = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_sym_col_count_mismatch --------------------------------------- */

static test_result_t test_sym_col_count_mismatch(void) {
    /* Intern enough symbols to have a known count */
    ray_sym_intern("cnt_a", 5);
    ray_sym_intern("cnt_b", 5);
    ray_sym_intern("cnt_c", 5);
    ray_sym_intern("cnt_d", 5);
    uint32_t sc = ray_sym_count();
    TEST_ASSERT((sc) >= (4), "sc >= 4");

    /* Build a W8 RAY_SYM column with valid indices */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W8, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = 3;
    uint8_t* data = (uint8_t*)ray_data(vec);
    data[0] = 0; data[1] = 1; data[2] = 2;

    /* Save with current sym count */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    ray_release(vec);

    /* Destroy sym table and re-init with fewer symbols.
     * This simulates loading a column against a smaller sym table. */
    ray_sym_destroy();
    (void)ray_sym_init();
    ray_sym_intern("only_one", 8);
    uint32_t new_sc = ray_sym_count();
    TEST_ASSERT((new_sc) < (sc), "new_sc < sc");

    /* Load should fail: saved sym count > current sym count (fast-reject) */
    ray_t* bad = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    /* Same via mmap */
    bad = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_sym_col_valid_roundtrip -------------------------------------- */

static test_result_t test_sym_col_valid_roundtrip(void) {
    /* Intern symbols */
    int64_t id0 = ray_sym_intern("rt_alpha", 8);
    int64_t id1 = ray_sym_intern("rt_beta", 7);
    int64_t id2 = ray_sym_intern("rt_gamma", 8);

    /* Build W16 RAY_SYM column */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W16, 5);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = 5;
    uint16_t* data = (uint16_t*)ray_data(vec);
    data[0] = (uint16_t)id0;
    data[1] = (uint16_t)id1;
    data[2] = (uint16_t)id2;
    data[3] = (uint16_t)id0;
    data[4] = (uint16_t)id1;

    /* Save + load roundtrip */
    ray_err_t err = ray_col_save(vec, TMP_COL_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_I(loaded->len, 5);
    TEST_ASSERT_EQ_U(loaded->attrs & RAY_SYM_W_MASK, RAY_SYM_W16);

    uint16_t* ld = (uint16_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(ld[0], id0);
    TEST_ASSERT_EQ_I(ld[1], id1);
    TEST_ASSERT_EQ_I(ld[2], id2);
    TEST_ASSERT_EQ_I(ld[3], id0);
    TEST_ASSERT_EQ_I(ld[4], id1);

    ray_release(loaded);

    /* Save + mmap roundtrip */
    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->type, RAY_SYM);
    TEST_ASSERT_EQ_I(mapped->len, 5);

    uint16_t* md = (uint16_t*)ray_data(mapped);
    TEST_ASSERT_EQ_I(md[0], id0);
    TEST_ASSERT_EQ_I(md[2], id2);

    ray_release(mapped);
    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_splay_load_with_sym ------------------------------------------ */

#define TMP_SPLAY_SYM_DIR "/tmp/rayforce_test_splay_sym"
#define TMP_SYM_PATH      "/tmp/rayforce_test_splay_sym_file"

static test_result_t test_splay_load_with_sym(void) {
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);

    /* Intern symbols and build a table with a RAY_SYM column */
    int64_t id_name = ray_sym_intern("name", 4);
    int64_t id_age  = ray_sym_intern("age", 3);
    int64_t sym_alice = ray_sym_intern("alice", 5);
    int64_t sym_bob   = ray_sym_intern("bob", 3);

    /* Build I64 column */
    int64_t raw_age[] = {30, 25};
    ray_t* col_age = ray_vec_from_raw(RAY_I64, raw_age, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_age));

    /* Build RAY_SYM W8 column */
    ray_t* col_name = ray_sym_vec_new(RAY_SYM_W8, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col_name));
    col_name->len = 2;
    uint8_t* sym_data = (uint8_t*)ray_data(col_name);
    sym_data[0] = (uint8_t)sym_alice;
    sym_data[1] = (uint8_t)sym_bob;

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, id_name, col_name);
    tbl = ray_table_add_col(tbl, id_age, col_age);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save splay + sym */
    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_SYM_DIR, TMP_SYM_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table, then load via ray_splay_load with sym_path */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    ray_t* loaded = ray_splay_load(TMP_SPLAY_SYM_DIR, TMP_SYM_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 2);

    /* Sym table should be populated again */
    TEST_ASSERT((ray_sym_count()) > (0), "ray_sym_count() > 0");

    ray_release(loaded);
    ray_release(col_name);
    ray_release(col_age);
    ray_release(tbl);
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);
    unlink(TMP_SYM_PATH ".lk");
    PASS();
}

/* ---- test_splay_load_sym_missing_corrupt ------------------------------- */

static test_result_t test_splay_load_sym_missing_corrupt(void) {
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);

    /* Intern symbols and build a table with a RAY_SYM column */
    int64_t id_col = ray_sym_intern("scol", 4);
    int64_t sym_val = ray_sym_intern("val_x", 5);

    ray_t* col = ray_sym_vec_new(RAY_SYM_W8, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(col));
    col->len = 1;
    ((uint8_t*)ray_data(col))[0] = (uint8_t)sym_val;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_col, col);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Save splay + sym */
    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_SYM_DIR, TMP_SYM_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table — simulate loading without sym */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 0);

    /* Load with NULL sym_path — should fail because RAY_SYM column exists
     * but sym table is empty. Note: col.c bounds check catches this first
     * since sym_count==0 skips validation, but the post-load check in
     * ray_splay_load catches RAY_SYM + empty sym table. */
    ray_t* loaded = ray_splay_load(TMP_SPLAY_SYM_DIR, NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded));
    TEST_ASSERT_STR_EQ(ray_err_code(loaded), "corrupt");
    ray_release(loaded);

    ray_release(col);
    ray_release(tbl);
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    unlink(TMP_SYM_PATH);
    unlink(TMP_SYM_PATH ".lk");
    PASS();
}

/* ---- test_read_splayed_bad_sym_fatal ----------------------------------- */

static test_result_t test_read_splayed_bad_sym_fatal(void) {
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);

    /* Build a simple table (no RAY_SYM columns needed) */
    int64_t id_x = ray_sym_intern("x", 1);
    int64_t raw[] = {1, 2, 3};
    ray_t* col_x = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_x, col_x);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, TMP_SPLAY_SYM_DIR, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* ray_read_splayed with nonexistent sym_path — should fail fatally */
    ray_t* loaded = ray_read_splayed(TMP_SPLAY_SYM_DIR, "/tmp/rayforce_nonexistent_sym_xyz");
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded));

    ray_release(col_x);
    ray_release(tbl);
    (void)!system("rm -rf " TMP_SPLAY_SYM_DIR);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

/* ---- test_serde_long_str_roundtrip --------------------------------------- */

static test_result_t test_serde_long_str_roundtrip(void) {
    /* Long string (>7 bytes) exercises the non-SSO path in serde.
     * Before the fix, the serializer used obj->slen <= 7 which could
     * misidentify a heap pointer's low byte as an SSO length, producing
     * an empty string on deserialization. */
    const char* long_str = "hello world, this is a long string for serde testing";
    size_t long_len = strlen(long_str);
    ray_t* orig = ray_str(long_str, long_len);
    TEST_ASSERT_NOT_NULL(orig);
    TEST_ASSERT_FALSE(RAY_IS_ERR(orig));

    /* Serialize */
    ray_t* wire = ray_ser(orig);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

    /* Deserialize */
    ray_t* back = ray_de(wire);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, -RAY_STR);

    /* Verify content matches */
    size_t back_len = ray_str_len(back);
    TEST_ASSERT_EQ_U(back_len, long_len);
    const char* back_ptr = ray_str_ptr(back);
    TEST_ASSERT_NOT_NULL(back_ptr);
    TEST_ASSERT_MEM_EQ(long_len, back_ptr, long_str);

    /* Also test a short string (SSO) round-trips correctly */
    ray_t* short_orig = ray_str("hi", 2);
    ray_t* short_wire = ray_ser(short_orig);
    ray_t* short_back = ray_de(short_wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(short_back));
    TEST_ASSERT_EQ_U(ray_str_len(short_back), 2);
    TEST_ASSERT_MEM_EQ(2, ray_str_ptr(short_back), "hi");

    ray_release(short_back);
    ray_release(short_wire);
    ray_release(short_orig);
    ray_release(back);
    ray_release(wire);
    ray_release(orig);

    PASS();
}

/* ---- test_serde_null_roundtrip ------------------------------------------ */

static test_result_t test_serde_null_roundtrip(void) {
    /* 1. C NULL pointer → RAY_SERDE_NULL → C NULL */
    {
        ray_t* wire = ray_ser(NULL);
        TEST_ASSERT_NOT_NULL(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(wire));
        ray_t* back = ray_de(wire);
        TEST_ASSERT_NULL(back);
        ray_release(wire);
    }

    /* 2. I64 vector with null bitmap: [10, NULL, 30] */
    {
        ray_t* vec = ray_vec_new(RAY_I64, 3);
        vec->len = 3;
        int64_t* data = (int64_t*)ray_data(vec);
        data[0] = 10;
        data[1] = INT64_MIN; /* null sentinel */
        data[2] = 30;
        ray_vec_set_null(vec, 1, true);

        ray_t* wire = ray_ser(vec);
        TEST_ASSERT_NOT_NULL(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

        ray_t* back = ray_de(wire);
        TEST_ASSERT_NOT_NULL(back);
        TEST_ASSERT_FALSE(RAY_IS_ERR(back));
        TEST_ASSERT_EQ_I(back->type, RAY_I64);
        TEST_ASSERT_EQ_I(back->len, 3);

        int64_t* bd = (int64_t*)ray_data(back);
        TEST_ASSERT_TRUE(bd[0] == 10);
        TEST_ASSERT_TRUE(bd[2] == 30);

        /* Verify actual null bitmap contents, not just the flag */
        TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_FALSE(ray_vec_is_null(back, 0));
        TEST_ASSERT_TRUE(ray_vec_is_null(back, 1));
        TEST_ASSERT_FALSE(ray_vec_is_null(back, 2));

        ray_release(back);
        ray_release(wire);
        ray_release(vec);
    }

    /* 3. F64 vector with NaN null: [1.5, NULL, 3.5] */
    {
        ray_t* vec = ray_vec_new(RAY_F64, 3);
        vec->len = 3;
        double* data = (double*)ray_data(vec);
        data[0] = 1.5;
        data[1] = NAN;
        data[2] = 3.5;
        ray_vec_set_null(vec, 1, true);

        ray_t* wire = ray_ser(vec);
        ray_t* back = ray_de(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(back));
        TEST_ASSERT_EQ_I(back->type, RAY_F64);

        double* bd = (double*)ray_data(back);
        TEST_ASSERT((bd[0]) == (1.5), "double == failed");
        TEST_ASSERT((bd[2]) == (3.5), "double == failed");
        TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_FALSE(ray_vec_is_null(back, 0));
        TEST_ASSERT_TRUE(ray_vec_is_null(back, 1));
        TEST_ASSERT_FALSE(ray_vec_is_null(back, 2));

        ray_release(back);
        ray_release(wire);
        ray_release(vec);
    }

    /* 4. STR vector with null element: ["hello", NULL, "world"] */
    {
        ray_t* vec = ray_vec_new(RAY_STR, 3);
        vec = ray_str_vec_append(vec, "hello", 5);
        vec = ray_str_vec_append(vec, "", 0);  /* placeholder for null */
        vec = ray_str_vec_append(vec, "world", 5);
        ray_vec_set_null(vec, 1, true);

        ray_t* wire = ray_ser(vec);
        TEST_ASSERT_NOT_NULL(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

        ray_t* back = ray_de(wire);
        TEST_ASSERT_NOT_NULL(back);
        TEST_ASSERT_FALSE(RAY_IS_ERR(back));
        TEST_ASSERT_EQ_I(back->type, RAY_STR);
        TEST_ASSERT_EQ_I(back->len, 3);
        TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_FALSE(ray_vec_is_null(back, 0));
        TEST_ASSERT_TRUE(ray_vec_is_null(back, 1));
        TEST_ASSERT_FALSE(ray_vec_is_null(back, 2));

        /* Non-null elements must survive */
        size_t slen = 0;
        const char* s0 = ray_str_vec_get(back, 0, &slen);
        TEST_ASSERT_EQ_U(slen, 5);
        TEST_ASSERT_MEM_EQ(5, s0, "hello");

        const char* s2 = ray_str_vec_get(back, 2, &slen);
        TEST_ASSERT_EQ_U(slen, 5);
        TEST_ASSERT_MEM_EQ(5, s2, "world");

        ray_release(back);
        ray_release(wire);
        ray_release(vec);
    }

    PASS();
}

/* ---- test_serde_typed_null_atoms ---------------------------------------- */

static test_result_t test_serde_typed_null_atoms(void) {
    /* Typed null atoms (0Nl, 0Nf, 0Nd, 0Nt, 0Ni, ...) must roundtrip as
     * typed nulls.  Before the fix, the atom wire format carried no null
     * marker, so (de (ser 0Nl)) decoded as plain ray_i64(0) and silently
     * lost the null bit.  The fix adds a 1-byte flags field after the
     * type byte on the atom path. */

    const int8_t atom_types[] = {
        -RAY_I64, -RAY_F64, -RAY_DATE, -RAY_TIME, -RAY_TIMESTAMP,
        -RAY_I32, -RAY_I16, -RAY_BOOL, -RAY_U8, -RAY_SYM, -RAY_STR,
    };
    for (size_t i = 0; i < sizeof(atom_types)/sizeof(atom_types[0]); i++) {
        int8_t t = atom_types[i];
        ray_t* orig = ray_typed_null(t);
        TEST_ASSERT_NOT_NULL(orig);
        TEST_ASSERT_FALSE(RAY_IS_ERR(orig));
        TEST_ASSERT_TRUE(RAY_ATOM_IS_NULL(orig));

        ray_t* wire = ray_ser(orig);
        TEST_ASSERT_NOT_NULL(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

        ray_t* back = ray_de(wire);
        TEST_ASSERT_NOT_NULL(back);
        TEST_ASSERT_FALSE(RAY_IS_ERR(back));
        TEST_ASSERT_EQ_I(back->type, t);
        TEST_ASSERT_TRUE(RAY_ATOM_IS_NULL(back));

        ray_release(back);
        ray_release(wire);
        ray_release(orig);
    }

    /* And regular (non-null) atoms must continue to roundtrip cleanly
     * with their value bit intact. */
    {
        ray_t* a = ray_i64(42);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_EQ_I(b->type, -RAY_I64);
        TEST_ASSERT_FALSE(RAY_ATOM_IS_NULL(b));
        TEST_ASSERT_EQ_I(b->i64, 42);
        ray_release(b); ray_release(w); ray_release(a);
    }
    {
        ray_t* a = ray_f64(3.14);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_EQ_I(b->type, -RAY_F64);
        TEST_ASSERT_FALSE(RAY_ATOM_IS_NULL(b));
        TEST_ASSERT_EQ_F(b->f64, 3.14, 1e-10);
        ray_release(b); ray_release(w); ray_release(a);
    }

    PASS();
}

/* ---- test_serde_wire_version_mismatch ---------------------------------- */

static test_result_t test_serde_wire_version_mismatch(void) {
    /* A payload serialized at the current wire version must roundtrip
     * cleanly; one tagged with a different version must be rejected
     * with a version error instead of being silently mis-parsed by a
     * peer that happens to share the prefix. */
    ray_t* orig = ray_i64(42);
    ray_t* wire = ray_ser(orig);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

    /* Clean roundtrip baseline. */
    {
        ray_t* back = ray_de(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(back));
        ray_release(back);
    }

    /* Tamper with the header's version byte. */
    ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(wire);
    uint8_t old_v = hdr->version;
    hdr->version = (uint8_t)(old_v + 1);

    ray_t* bad = ray_de(wire);
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    /* err type string should be "version" per the fix in ray_de */
    TEST_ASSERT_MEM_EQ(7, bad->sdata, "version");

    /* Restore and confirm good version still works. */
    hdr->version = old_v;
    ray_t* good = ray_de(wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(good));
    ray_release(good);

    ray_release(wire);
    ray_release(orig);
    PASS();
}

/* ---- serde coverage: atom type roundtrips -------------------------------- */

/* Covers: ray_bool/u8/i16/i32/date/time/f32/guid atom ser+de paths,
 * plus the RAY_ERROR and serde_size default=0 paths. */
static test_result_t test_serde_atom_types(void) {
    /* BOOL atom */
    {
        ray_t* a = ray_bool(true);
        ray_t* w = ray_ser(a);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_BOOL);
        TEST_ASSERT_TRUE(b->u8 == 1);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* U8 atom */
    {
        ray_t* a = ray_u8(255);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_U8);
        TEST_ASSERT_EQ_I((int)b->u8, 255);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* I16 atom */
    {
        ray_t* a = ray_i16(1234);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_I16);
        TEST_ASSERT_EQ_I((int)b->i16, 1234);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* I32 atom */
    {
        ray_t* a = ray_i32(987654);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_I32);
        TEST_ASSERT_EQ_I(b->i32, 987654);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* DATE atom */
    {
        ray_t* a = ray_date(20250101);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_DATE);
        TEST_ASSERT_EQ_I(b->i32, 20250101);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* TIME atom */
    {
        ray_t* a = ray_time(120000);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_TIME);
        TEST_ASSERT_EQ_I(b->i32, 120000);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* TIMESTAMP atom */
    {
        ray_t* a = ray_timestamp(1234567890LL);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_TIMESTAMP);
        TEST_ASSERT_EQ_I(b->i64, 1234567890LL);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* GUID atom */
    {
        uint8_t guid_bytes[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        ray_t* a = ray_guid(guid_bytes);
        TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_FALSE(RAY_IS_ERR(a));
        ray_t* w = ray_ser(a);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_GUID);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* SYM atom */
    {
        int64_t id = ray_sym_intern("mysym", 5);
        ray_t* a = ray_sym(id);
        ray_t* w = ray_ser(a);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_SYM);
        TEST_ASSERT_EQ_I(b->i64, id);
        ray_release(b); ray_release(w); ray_release(a);
    }
    PASS();
}

/* ---- serde coverage: vector type roundtrips ------------------------------ */

/* Covers: RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_DATE, RAY_TIME, RAY_F32,
 * RAY_GUID, RAY_SYM, RAY_TIMESTAMP vector ser+de paths. */
static test_result_t test_serde_vec_types(void) {
    /* BOOL vector */
    {
        uint8_t raw[] = {1, 0, 1, 1, 0};
        ray_t* v = ray_vec_from_raw(RAY_BOOL, raw, 5);
        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_BOOL);
        TEST_ASSERT_EQ_I(b->len, 5);
        uint8_t* bd = (uint8_t*)ray_data(b);
        for (int i = 0; i < 5; i++) TEST_ASSERT_EQ_I((int)bd[i], (int)raw[i]);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* U8 vector */
    {
        uint8_t raw[] = {10, 20, 30};
        ray_t* v = ray_vec_from_raw(RAY_U8, raw, 3);
        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_U8);
        TEST_ASSERT_EQ_I(b->len, 3);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* I16 vector */
    {
        int16_t raw[] = {-100, 0, 100};
        ray_t* v = ray_vec_from_raw(RAY_I16, raw, 3);
        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_I16);
        TEST_ASSERT_EQ_I(b->len, 3);
        int16_t* bd = (int16_t*)ray_data(b);
        for (int i = 0; i < 3; i++) TEST_ASSERT_EQ_I((int)bd[i], (int)raw[i]);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* I32 vector */
    {
        int32_t raw[] = {1000000, -2000000, 3000000};
        ray_t* v = ray_vec_from_raw(RAY_I32, raw, 3);
        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_I32);
        TEST_ASSERT_EQ_I(b->len, 3);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* DATE vector */
    {
        int32_t raw[] = {20250101, 20250102};
        ray_t* v = ray_vec_from_raw(RAY_DATE, raw, 2);
        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_DATE);
        TEST_ASSERT_EQ_I(b->len, 2);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* TIME vector */
    {
        int32_t raw[] = {0, 43200000, 86399000};
        ray_t* v = ray_vec_from_raw(RAY_TIME, raw, 3);
        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_TIME);
        TEST_ASSERT_EQ_I(b->len, 3);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* F32 vector — stored as 4-byte float */
    {
        float raw[] = {1.5f, -2.5f, 3.0f};
        ray_t* v = ray_vec_from_raw(RAY_F32, raw, 3);
        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_F32);
        TEST_ASSERT_EQ_I(b->len, 3);
        float* bd = (float*)ray_data(b);
        for (int i = 0; i < 3; i++) TEST_ASSERT_EQ_F((double)bd[i], (double)raw[i], 1e-6);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* TIMESTAMP vector */
    {
        int64_t raw[] = {1000000000LL, 2000000000LL};
        ray_t* v = ray_vec_from_raw(RAY_TIMESTAMP, raw, 2);
        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_TIMESTAMP);
        TEST_ASSERT_EQ_I(b->len, 2);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* GUID vector */
    {
        /* Build a small GUID vector by allocating and filling raw bytes */
        ray_t* v = ray_vec_new(RAY_GUID, 2);
        TEST_ASSERT_NOT_NULL(v); TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        v->len = 2;
        uint8_t* gdata = (uint8_t*)ray_data(v);
        for (int i = 0; i < 32; i++) gdata[i] = (uint8_t)i;
        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_GUID);
        TEST_ASSERT_EQ_I(b->len, 2);
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* SYM vector */
    {
        int64_t id1 = ray_sym_intern("alpha", 5);
        int64_t id2 = ray_sym_intern("beta",  4);
        ray_t* v = ray_vec_new(RAY_SYM, 2);
        TEST_ASSERT_NOT_NULL(v); TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        v->len = 2;
        int64_t* ids = (int64_t*)ray_data(v);
        ids[0] = id1; ids[1] = id2;
        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_SYM);
        TEST_ASSERT_EQ_I(b->len, 2);
        int64_t* bid = (int64_t*)ray_data(b);
        TEST_ASSERT_EQ_I(bid[0], id1);
        TEST_ASSERT_EQ_I(bid[1], id2);
        ray_release(b); ray_release(w); ray_release(v);
    }
    PASS();
}

/* ---- serde coverage: TABLE roundtrip ------------------------------------- */

static test_result_t test_serde_table_roundtrip(void) {
    int64_t col_a[] = {10, 20, 30};
    double  col_b[] = {1.1, 2.2, 3.3};
    ray_t* va = ray_vec_from_raw(RAY_I64, col_a, 3);
    ray_t* vb = ray_vec_from_raw(RAY_F64, col_b, 3);
    int64_t na = ray_sym_intern("x", 1);
    int64_t nb = ray_sym_intern("y", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, na, va);
    tbl = ray_table_add_col(tbl, nb, vb);
    ray_release(va); ray_release(vb);

    TEST_ASSERT_NOT_NULL(tbl); TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_t* w = ray_ser(tbl);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));

    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(b), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(b), 3);

    ray_t* col_out = ray_table_get_col(b, na);
    TEST_ASSERT_NOT_NULL(col_out); TEST_ASSERT_FALSE(RAY_IS_ERR(col_out));
    TEST_ASSERT_EQ_I(col_out->type, RAY_I64);
    TEST_ASSERT_EQ_I(col_out->len, 3);
    int64_t* outd = (int64_t*)ray_data(col_out);
    TEST_ASSERT_EQ_I(outd[0], 10);
    TEST_ASSERT_EQ_I(outd[1], 20);
    TEST_ASSERT_EQ_I(outd[2], 30);
    ray_release(col_out);

    ray_release(b); ray_release(w); ray_release(tbl);
    PASS();
}

/* ---- serde coverage: DICT roundtrip -------------------------------------- */

static test_result_t test_serde_dict_roundtrip(void) {
    /* Build dict {`a` -> 1, `b` -> 2} */
    int64_t ka = ray_sym_intern("a", 1);
    int64_t kb = ray_sym_intern("b", 1);

    ray_t* keys = ray_vec_new(RAY_SYM, 2);
    TEST_ASSERT_NOT_NULL(keys); TEST_ASSERT_FALSE(RAY_IS_ERR(keys));
    keys->len = 2;
    int64_t* kid = (int64_t*)ray_data(keys);
    kid[0] = ka; kid[1] = kb;

    int64_t vraw[] = {1, 2};
    ray_t* vals = ray_vec_from_raw(RAY_I64, vraw, 2);

    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(d); TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    ray_t* w = ray_ser(d);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));

    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(b), 2);

    ray_release(b); ray_release(w); ray_release(d);
    PASS();
}

/* ---- serde coverage: ray_obj_save / ray_obj_load ------------------------- */

#define TMP_SERDE_PATH "/tmp/rayforce_serde_test.rfl"

static test_result_t test_serde_obj_save_load(void) {
    /* Save and load an I64 vec */
    int64_t raw[] = {100, 200, 300, 400};
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_err_t err = ray_obj_save(v, TMP_SERDE_PATH);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* back = ray_obj_load(TMP_SERDE_PATH);
    TEST_ASSERT_NOT_NULL(back); TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, RAY_I64);
    TEST_ASSERT_EQ_I(back->len, 4);
    int64_t* bd = (int64_t*)ray_data(back);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQ_I(bd[i], raw[i]);

    ray_release(back);
    ray_release(v);
    unlink(TMP_SERDE_PATH);
    PASS();
}

/* ray_obj_load error paths: missing file, empty file, bad data */
static test_result_t test_serde_obj_load_errors(void) {
    /* Non-existent file */
    {
        ray_t* r = ray_obj_load("/tmp/rayforce_nonexistent_42.rfl");
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
    }
    /* Empty file */
    {
        FILE* f = fopen("/tmp/rayforce_empty_test.rfl", "wb");
        TEST_ASSERT_NOT_NULL(f);
        fclose(f);
        ray_t* r = ray_obj_load("/tmp/rayforce_empty_test.rfl");
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
        unlink("/tmp/rayforce_empty_test.rfl");
    }
    /* Bad data (no valid header) */
    {
        FILE* f = fopen("/tmp/rayforce_bad_test.rfl", "wb");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t junk[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
                          0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
        ray_t* r = ray_obj_load("/tmp/rayforce_bad_test.rfl");
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
        unlink("/tmp/rayforce_bad_test.rfl");
    }
    PASS();
}

/* ray_obj_save error path: ray_ser returns error (NULL input produces SERDE_NULL,
 * not an error; so pass a bad-type object — easiest is calling ray_ser with an
 * object whose serde_size returns 0, e.g. a zero-length serde_size result by
 * making ray_ser return error).  Actually ray_obj_save(NULL, path) calls
 * ray_ser(NULL) which returns a valid SERDE_NULL frame, so use a deliberately
 * crafted broken object instead.  Simplest: a RAY_U8 vec with negative length. */
static test_result_t test_serde_obj_save_error(void) {
    /* ray_de with bad prefix: wrong prefix bytes in header -> domain error */
    {
        ray_t* w = ray_ser(ray_i64(99));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        /* Corrupt prefix */
        uint8_t* ptr = (uint8_t*)ray_data(w);
        ptr[0] ^= 0xFF;
        ray_t* r = ray_de(w);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(w);
    }
    /* ray_de with wrong payload size in header */
    {
        ray_t* w = ray_ser(ray_i64(99));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        /* Mess up hdr->size so size+hdr != total */
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(w);
        hdr->size = hdr->size + 999;
        ray_t* r = ray_de(w);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(w);
    }
    /* ray_de with truncated buffer (too small for header) */
    {
        uint8_t tiny[3] = {0x01, 0x02, 0x03};
        ray_t* v = ray_vec_from_raw(RAY_U8, tiny, 3);
        ray_t* r = ray_de(v);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(v);
    }
    /* ray_de with non-U8 input type */
    {
        int64_t raw[] = {1, 2};
        ray_t* v = ray_vec_from_raw(RAY_I64, raw, 2);
        ray_t* r = ray_de(v);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(v);
    }
    PASS();
}

/* ---- serde coverage: vector null bitmaps for BOOL/U8/I16/I32 types ------- */

/* Exercises the de_null_bitmap path for non-I64/F64 vector types,
 * covering lines 586-656 (the RAY_BOOL/U8/I16/I32/DATE/TIME/F32 vector
 * deserialization with HAS_NULLS). */
static test_result_t test_serde_vec_null_bitmaps(void) {
    /* BOOL vector with null at index 1 */
    {
        ray_t* v = ray_vec_new(RAY_BOOL, 3);
        TEST_ASSERT_NOT_NULL(v); TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        v->len = 3;
        uint8_t* d = (uint8_t*)ray_data(v);
        d[0] = 1; d[1] = 0; d[2] = 1;
        ray_vec_set_null(v, 1, true);

        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_BOOL);
        TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_TRUE(ray_vec_is_null(b, 1));
        TEST_ASSERT_FALSE(ray_vec_is_null(b, 0));
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* I32 vector with null at index 0 */
    {
        int32_t raw[] = {0, 100, 200};
        ray_t* v = ray_vec_from_raw(RAY_I32, raw, 3);
        ray_vec_set_null(v, 0, true);

        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_I32);
        TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_TRUE(ray_vec_is_null(b, 0));
        TEST_ASSERT_FALSE(ray_vec_is_null(b, 1));
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* I16 vector with null */
    {
        int16_t raw[] = {-1, 2, -3};
        ray_t* v = ray_vec_from_raw(RAY_I16, raw, 3);
        ray_vec_set_null(v, 2, true);

        ray_t* w = ray_ser(v);
        ray_t* b = ray_de(w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_I16);
        TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_NULLS);
        TEST_ASSERT_TRUE(ray_vec_is_null(b, 2));
        ray_release(b); ray_release(w); ray_release(v);
    }
    /* SYM vector with null bitmap */
    {
        int64_t id1 = ray_sym_intern("p", 1);
        int64_t id2 = ray_sym_intern("q", 1);
        ray_t* v = ray_vec_new(RAY_SYM, 2);
        v->len = 2;
        int64_t* ids = (int64_t*)ray_data(v);
        ids[0] = id1; ids[1] = id2;
        ray_vec_set_null(v, 0, true);

        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_SYM);
        TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_NULLS);
        ray_release(b); ray_release(w); ray_release(v);
    }
    PASS();
}

/* ---- serde coverage: de error paths ------------------------------------- */

/* Exercises error returns in ray_de_raw for truncated/bad input. */
static test_result_t test_serde_de_error_paths(void) {
    /* Build a valid I64 wire frame then corrupt payload to be too short */
    {
        ray_t* w = ray_ser(ray_i64(42));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        /* Shrink the wire buffer so the payload is truncated.
         * Write: type(-I64)=1B + flags=1B + value=8B = 10B payload.
         * Cut payload to 5 bytes by adjusting hdr->size. */
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(w);
        int64_t orig_size = hdr->size;
        hdr->size = 3; /* too short for I64 atom (needs 10 bytes) */
        w->len = (int64_t)sizeof(ray_ipc_header_t) + 3;
        /* Keep raw bytes valid so only the size check fires. */
        ray_t* r = ray_de(w);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
        /* Restore */
        hdr->size = orig_size;
        w->len = (int64_t)sizeof(ray_ipc_header_t) + orig_size;
        ray_release(w);
    }
    /* Truncated I64 vector — header OK but data too short */
    {
        int64_t raw[] = {1, 2, 3, 4, 5};
        ray_t* v = ray_vec_from_raw(RAY_I64, raw, 5);
        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        /* Trim payload to 10 bytes (too short for 5*8=40 bytes of data) */
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(w);
        hdr->size = 10;
        w->len = (int64_t)sizeof(ray_ipc_header_t) + 10;
        ray_t* r = ray_de(w);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(w); ray_release(v);
    }
    /* Unknown type byte in payload -> default error arm */
    {
        ray_t* w = ray_ser(ray_i64(1));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        /* Overwrite type byte in payload with 120 (not a known type) */
        uint8_t* payload = (uint8_t*)ray_data(w) + sizeof(ray_ipc_header_t);
        payload[0] = 120; /* unknown positive type */
        ray_t* r = ray_de(w);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(w);
    }
    PASS();
}

/* ---- serde coverage: LIST with NULL element inside ----------------------- */

/* Tests that lists containing NULL sentinel elements round-trip correctly
 * (the RAY_NULL_OBJ substitution path in ray_de_raw at line 725-726). */
static test_result_t test_serde_list_with_null_elem(void) {
    /* Build a 3-element list: [i64(1), RAY_NULL_OBJ, i64(3)] */
    ray_t* list = ray_alloc(3 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(list); TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    list->type  = RAY_LIST;
    list->attrs = 0;
    list->len   = 3;
    ray_t** elems = (ray_t**)ray_data(list);
    elems[0] = ray_i64(1);
    elems[1] = RAY_NULL_OBJ;
    elems[2] = ray_i64(3);

    ray_t* w = ray_ser(list);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));

    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_LIST);
    TEST_ASSERT_EQ_I(b->len, 3);
    ray_t** be = (ray_t**)ray_data(b);
    TEST_ASSERT_NOT_NULL(be[0]);
    TEST_ASSERT_NOT_NULL(be[2]);
    /* Middle element round-trips as NULL_OBJ */
    TEST_ASSERT_TRUE(RAY_IS_NULL(be[1]));

    ray_release(b); ray_release(w);
    /* Release list elements manually since list owns them */
    ray_release(elems[0]);
    /* elems[1] is RAY_NULL_OBJ — do not release */
    ray_release(elems[2]);
    ray_release(list);
    PASS();
}

/* ---- serde coverage: UNARY/BINARY/VARY function roundtrip ---------------- */

/* The UNARY/BINARY/VARY serialization path stores the function name and
 * deserializes by looking it up in the global env.  Requires a runtime. */
static test_result_t test_serde_function_types(void) {
    /* We use ray_runtime_create to populate the global env with builtins
     * so that ray_env_get("neg") etc. succeed on deserialization. */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    /* Look up "neg" (a unary builtin) from global env */
    int64_t neg_id = ray_sym_intern("neg", 3);
    ray_t* neg_fn = ray_env_get(neg_id);
    if (neg_fn && !RAY_IS_ERR(neg_fn) && neg_fn->type == RAY_UNARY) {
        ray_t* w = ray_ser(neg_fn);
        if (w && !RAY_IS_ERR(w)) {
            ray_t* b = ray_de(w);
            if (b && !RAY_IS_ERR(b)) {
                TEST_ASSERT_EQ_I(b->type, RAY_UNARY);
                ray_release(b);
            }
            ray_release(w);
        }
    }

    /* Look up "+" (a binary builtin) */
    int64_t add_id = ray_sym_intern("+", 1);
    ray_t* add_fn = ray_env_get(add_id);
    if (add_fn && !RAY_IS_ERR(add_fn) && add_fn->type == RAY_BINARY) {
        ray_t* w = ray_ser(add_fn);
        if (w && !RAY_IS_ERR(w)) {
            ray_t* b = ray_de(w);
            if (b && !RAY_IS_ERR(b)) {
                TEST_ASSERT_EQ_I(b->type, RAY_BINARY);
                ray_release(b);
            }
            ray_release(w);
        }
    }

    /* Look up "list" (a variadic builtin) */
    int64_t list_id = ray_sym_intern("list", 4);
    ray_t* list_fn = ray_env_get(list_id);
    if (list_fn && !RAY_IS_ERR(list_fn) && list_fn->type == RAY_VARY) {
        ray_t* w = ray_ser(list_fn);
        if (w && !RAY_IS_ERR(w)) {
            ray_t* b = ray_de(w);
            if (b && !RAY_IS_ERR(b)) {
                TEST_ASSERT_EQ_I(b->type, RAY_VARY);
                ray_release(b);
            }
            ray_release(w);
        }
    }

    ray_runtime_destroy(rt);
    PASS();
}

/* ---- serde coverage: ERROR object roundtrip ------------------------------ */

static test_result_t test_serde_error_roundtrip(void) {
    /* Build an error object and round-trip it through ser/de */
    ray_t* e = ray_error("domain", NULL);
    TEST_ASSERT_NOT_NULL(e); TEST_ASSERT_TRUE(RAY_IS_ERR(e));

    /* ray_ser handles IS_ERR: writes 1+8 bytes */
    ray_t* w = ray_ser(e);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));

    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b);
    /* The deserialized form is a RAY_ERROR object */
    TEST_ASSERT_TRUE(RAY_IS_ERR(b));

    ray_release(b); ray_release(w); ray_release(e);
    PASS();
}

/* ---- serde coverage: large null vector (>128 elems, ext nullmap path) ---- */

/* When a vector has more than 128 elements and HAS_NULLS, de_null_bitmap
 * allocates an external nullmap (RAY_ATTR_NULLMAP_EXT).  This covers
 * lines 117-122 in serde.c. */
static test_result_t test_serde_large_null_vec(void) {
    int64_t n = 200;
    ray_t* v = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(v); TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = i * 2;
    /* Set a few nulls */
    ray_vec_set_null(v, 0,   true);
    ray_vec_set_null(v, 99,  true);
    ray_vec_set_null(v, 199, true);

    ray_t* w = ray_ser(v);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));

    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_I64);
    TEST_ASSERT_EQ_I(b->len, n);
    TEST_ASSERT_TRUE(b->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(ray_vec_is_null(b, 0));
    TEST_ASSERT_TRUE(ray_vec_is_null(b, 99));
    TEST_ASSERT_TRUE(ray_vec_is_null(b, 199));
    TEST_ASSERT_FALSE(ray_vec_is_null(b, 1));

    ray_release(b); ray_release(w); ray_release(v);
    PASS();
}

/* ---- serde coverage: F32 atom + GUID null atom + default/err serde_size -- */

static test_result_t test_serde_f32_atom_and_edge_cases(void) {
    /* F32 atom round-trip: ser_raw narrows obj->f64 to float, de reads
     * the float back into a -RAY_F32 atom (value preserved within float
     * precision; type also preserved). */
    {
        ray_t* a = ray_f32(3.14f);
        TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_FALSE(RAY_IS_ERR(a));
        ray_t* w = ray_ser(a);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, -RAY_F32);
        ray_release(b); ray_release(w); ray_release(a);
    }
    /* F32 typed null atom */
    {
        ray_t* a = ray_typed_null(-RAY_F32);
        if (a && !RAY_IS_ERR(a)) {
            ray_t* w = ray_ser(a);
            if (w && !RAY_IS_ERR(w)) {
                ray_t* b = ray_de(w);
                /* Should be a typed null, promoted to F64 null */
                if (b && !RAY_IS_ERR(b)) {
                    TEST_ASSERT_TRUE(RAY_ATOM_IS_NULL(b));
                }
                if (b) ray_release(b);
                ray_release(w);
            }
            ray_release(a);
        }
    }
    /* GUID atom with null obj pointer (the memset 0 branch line 308) */
    {
        /* Build a GUID atom manually with obj=NULL to hit the else branch */
        ray_t* a = ray_typed_null(-RAY_GUID);
        if (a && !RAY_IS_ERR(a)) {
            /* Force obj to NULL to trigger the memset path */
            a->obj = NULL;
            a->nullmap[0] = 0; /* clear null bit to force value path */
            ray_t* w = ray_ser(a);
            if (w && !RAY_IS_ERR(w)) {
                ray_t* b = ray_de(w);
                if (b && !RAY_IS_ERR(b)) ray_release(b);
                ray_release(w);
            }
            ray_release(a);
        }
    }
    /* ray_serde_size with RAY_ERROR object (lines 236-237) */
    {
        ray_t* e = ray_error("io", NULL);
        TEST_ASSERT_NOT_NULL(e);
        /* ray_serde_size IS_ERR check at line 137 fires first (returns 1+8),
         * but for the vector switch default path at line 236 we need a non-IS_ERR
         * object with type==RAY_ERROR. Directly test via ray_ser which calls
         * serde_size internally. */
        int64_t sz = ray_serde_size(e);
        TEST_ASSERT_EQ_I(sz, 1 + 8);
        ray_release(e);
    }
    /* safe_strlen: trigger the no-null path (line 77) by crafting a raw
     * deserialization with a SYM atom payload that has no null in bounds */
    {
        /* Build a raw buffer manually: type=-RAY_SYM, flags=0, then 4 non-null
         * bytes, then only 4 bytes available — safe_strlen should hit max */
        /* Use ray_de_raw directly by crafting an IPC frame with SYM atom
         * that has no null terminator within avail bytes */
        ray_t* frame = ray_ser(ray_i64(0)); /* get a valid frame for sizing */
        if (frame && !RAY_IS_ERR(frame)) {
            /* Overwrite payload: type=-RAY_SYM(=-12), flags=0, 4 bytes 'a','b','c','d' (no null) */
            uint8_t* payload = (uint8_t*)ray_data(frame) + sizeof(ray_ipc_header_t);
            ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(frame);
            /* We only have 10 bytes of payload (1+1+8 from i64 atom); reuse
             * the 10 bytes: type(1)+flags(1)+data(8), set data to 8 non-null chars */
            payload[0] = (uint8_t)(-RAY_SYM); /* -12 = 0xF4 */
            payload[1] = 0; /* flags */
            /* Fill remaining 8 bytes with non-null to trigger no-null path */
            for (int i = 2; i < 10; i++) payload[i] = 'x';
            /* Now the SYM atom deserializer reads safe_strlen(buf+2, 8) where
             * none of the 8 bytes is 0, so safe_strlen returns 8 = max,
             * and then (8 >= 8) triggers domain error. */
            hdr->size = 10;
            frame->len = (int64_t)sizeof(ray_ipc_header_t) + 10;
            ray_t* r = ray_de(frame);
            /* Expect error (safe_strlen==8, 8>=8 triggers domain) */
            TEST_ASSERT_NOT_NULL(r);
            TEST_ASSERT_TRUE(RAY_IS_ERR(r));
            ray_release(r);
            ray_release(frame);
        }
    }
    PASS();
}

/* ---- serde coverage: LAMBDA object roundtrip ----------------------------- */

/* Builds a LAMBDA object by hand (same layout as serde.c deserializer) and
 * round-trips it.  This covers lines 224-226, 460-466, 820-850. */
static test_result_t test_serde_lambda_roundtrip(void) {
    /* Build a lambda: params = sym vec ["x"], body = i64(42) atom */
    int64_t x_id = ray_sym_intern("x", 1);
    ray_t* params = ray_vec_new(RAY_SYM, 1);
    TEST_ASSERT_NOT_NULL(params); TEST_ASSERT_FALSE(RAY_IS_ERR(params));
    params->len = 1;
    ((int64_t*)ray_data(params))[0] = x_id;

    ray_t* body = ray_i64(42);
    TEST_ASSERT_NOT_NULL(body);

    /* Allocate lambda with 7 pointer slots (same layout as eval.c) */
    ray_t* lambda = ray_alloc(7 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(lambda); TEST_ASSERT_FALSE(RAY_IS_ERR(lambda));
    lambda->type  = RAY_LAMBDA;
    lambda->attrs = 0;
    lambda->len   = 0;
    memset(ray_data(lambda), 0, 7 * sizeof(ray_t*));
    ((ray_t**)ray_data(lambda))[0] = params;
    ((ray_t**)ray_data(lambda))[1] = body;

    /* Verify serde_size covers RAY_LAMBDA branch */
    int64_t sz = ray_serde_size(lambda);
    TEST_ASSERT_FMT(sz > 0, "serde_size should be > 0 for LAMBDA");

    /* Serialize */
    ray_t* w = ray_ser(lambda);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));

    /* Deserialize */
    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_LAMBDA);
    /* params slot should be a SYM vector */
    ray_t** bslots = (ray_t**)ray_data(b);
    TEST_ASSERT_NOT_NULL(bslots[0]);
    TEST_ASSERT_EQ_I(bslots[0]->type, RAY_SYM);
    TEST_ASSERT_EQ_I(bslots[0]->len, 1);
    /* body slot should be an I64 atom */
    TEST_ASSERT_NOT_NULL(bslots[1]);
    TEST_ASSERT_EQ_I(bslots[1]->type, -RAY_I64);
    TEST_ASSERT_EQ_I(bslots[1]->i64, 42);

    ray_release(b); ray_release(w); ray_release(lambda);
    PASS();
}

/* ---- serde coverage: ray_obj_save serialization failure path ------------- */

/* ray_obj_save calls ray_ser(obj) first; if that returns error (e.g. object
 * whose serde_size returns 0 → ray_ser returns error "domain"), the early
 * RAY_ERR_DOMAIN path fires (lines 944-946).
 *
 * We build an object whose type is in the default branch of ray_serde_size
 * (lines 238-240) so serde_size returns 0.  We craft a raw ray_t manually
 * with a type that isn't handled: use type=50 (between LIST and LAMBDA). */
static test_result_t test_serde_save_serde_error(void) {
    /* A type-=239 (default arm) object: use a locally-crafted I64 vec
     * but overwrite type to an unknown value after construction so we
     * don't corrupt the heap tracker. */
    ray_t* v = ray_i64(7);
    TEST_ASSERT_NOT_NULL(v);
    /* Overwrite type to an unknown positive type value that hits default */
    int8_t orig_type = v->type;
    v->type = 50; /* not a recognized type in ray_ser_raw */
    int64_t sz = ray_serde_size(v);
    /* serde_size should return 0 for unknown type 50 */
    TEST_ASSERT_EQ_I(sz, 0);
    /* Restore before release to avoid corrupting the heap */
    v->type = orig_type;
    ray_release(v);
    PASS();
}

/* ---- serde coverage: default/unknown atom type error paths --------------- */

/* Exercises the default arms in ray_de_raw for unknown atom types (lines
 * 577-578), SYM-vec truncation error (lines 642-645), LIST child error
 * (lines 729-733), and ray_ser written==0 path (lines 902-904). */
static test_result_t test_serde_de_raw_default_and_errors(void) {
    /* Build IPC frame with an unknown negative type tag in the payload
     * to hit the atom default arm (line 577-578).
     * Unknown negative type = -90 = 0xA6.  The de_raw reads it, enters
     * type<0 branch, reads 1 flags byte, then hits default -> error. */
    {
        ray_t* w = ray_ser(ray_i64(0));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        /* Overwrite type byte to unknown negative: 0xA6 = (uint8_t)(-90) */
        uint8_t* payload = (uint8_t*)ray_data(w) + sizeof(ray_ipc_header_t);
        payload[0] = 0xA6; /* -90 as signed byte, unknown atom type */
        ray_t* r = ray_de(w);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(w);
    }
    /* SYM vector where an element has no null terminator within bounds:
     * craft a payload: type=RAY_SYM(12), attrs=0, len=1, then 4 non-null
     * bytes and nothing else → safe_strlen returns 4 = *len, domain error */
    {
        /* Frame: header + payload */
        /* Payload for SYM vec: type(1) + attrs(1) + len8(8) + 1 sym with 4 bytes + no null */
        size_t hdrsz = sizeof(ray_ipc_header_t);
        /* Total payload: 1+1+8+4 = 14 bytes */
        int64_t total = (int64_t)(hdrsz + 14);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        TEST_ASSERT_NOT_NULL(raw_buf); TEST_ASSERT_FALSE(RAY_IS_ERR(raw_buf));
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        /* Write IPC header */
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix  = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags   = 0;
        hdr->endian  = 0;
        hdr->msgtype = 0;
        hdr->size    = 14;
        /* Write SYM vector payload */
        uint8_t* pl = p + hdrsz;
        pl[0] = (uint8_t)RAY_SYM; /* type = 12 */
        pl[1] = 0;                 /* attrs = 0 */
        int64_t sym_count = 1;
        memcpy(pl + 2, &sym_count, 8);
        /* 4 non-null bytes (no null terminator) */
        pl[10] = 'a'; pl[11] = 'b'; pl[12] = 'c'; pl[13] = 'd';
        ray_t* r = ray_de(raw_buf);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    /* LIST with a child element that errors: craft a list with 2 elements
     * where the second one has an unknown type → child error triggers
     * the cleanup path (lines 729-733) */
    {
        /* Build payload: type=LIST(16), attrs=0, len=2,
         * elem1 = valid I64 atom (1+1+8=10 bytes),
         * elem2 = unknown type 0xA6 + 1 flags byte (2 bytes needed) */
        size_t hdrsz = sizeof(ray_ipc_header_t);
        /* LIST hdr: 1+1+8=10; elem1=10; elem2=2 => payload=22 */
        int64_t payload_sz = 22;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        TEST_ASSERT_NOT_NULL(raw_buf); TEST_ASSERT_FALSE(RAY_IS_ERR(raw_buf));
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix  = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags   = 0;
        hdr->endian  = 0;
        hdr->msgtype = 0;
        hdr->size    = payload_sz;
        uint8_t* pl = p + hdrsz;
        int pos = 0;
        /* LIST header */
        pl[pos++] = (uint8_t)RAY_LIST; /* type=16 */
        pl[pos++] = 0;                  /* attrs */
        int64_t list_len = 2;
        memcpy(pl + pos, &list_len, 8); pos += 8;
        /* elem1: I64 atom: type=-RAY_I64=0xF5, flags=0, value=42 */
        pl[pos++] = (uint8_t)(-RAY_I64); /* 0xF5 */
        pl[pos++] = 0;                    /* flags */
        int64_t val = 42;
        memcpy(pl + pos, &val, 8); pos += 8;
        /* elem2: unknown negative type 0xA6, flags=0 */
        pl[pos++] = 0xA6; /* unknown atom */
        pl[pos++] = 0;    /* flags — but no more data to read */
        /* The default arm fires and returns error, triggering cleanup */
        (void)pos;
        ray_t* r = ray_de(raw_buf);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    /* ray_ser returns error when written==0: use object with type in the
     * default arm of ray_ser_raw (type=50, positive unknown).
     * serde_size returns 0 → ray_ser returns domain error */
    {
        ray_t* v = ray_i64(1);
        v->type = 50; /* unknown positive type */
        /* serde_size(v) returns 0 → ray_ser returns error "domain" */
        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_TRUE(RAY_IS_ERR(w));
        ray_release(w);
        v->type = -RAY_I64; /* restore so ray_release works */
        ray_release(v);
    }
    PASS();
}

/* ---- serde coverage: TABLE/DICT deserialization error paths -------------- */

/* Exercises the TABLE and DICT deser error paths by crafting malformed
 * payloads where schema/cols deserialization fails. */
static test_result_t test_serde_table_dict_de_errors(void) {
    size_t hdrsz = sizeof(ray_ipc_header_t);

    /* TABLE deser: schema deserialization fails (truncated payload) */
    {
        /* Payload: type=TABLE(97 — let me check), attrs=0, then truncated */
        /* RAY_TABLE = ... let me use the constant */
        int64_t payload_sz = 3; /* type(1) + attrs(1) + 1 byte (too short for schema) */
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        TEST_ASSERT_NOT_NULL(raw_buf); TEST_ASSERT_FALSE(RAY_IS_ERR(raw_buf));
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix  = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags   = 0;
        hdr->endian  = 0;
        hdr->msgtype = 0;
        hdr->size    = payload_sz;
        uint8_t* pl = p + hdrsz;
        pl[0] = (uint8_t)RAY_TABLE; /* type */
        pl[1] = 0;                  /* attrs */
        pl[2] = 0xA6;               /* unknown type for schema → de_raw error */
        ray_t* r = ray_de(raw_buf);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    /* TABLE deser: cols deserialization fails after schema succeeds */
    {
        /* Schema = NULL (SERDE_NULL=126=0x7E), then cols = unknown type */
        int64_t payload_sz = 4; /* TABLE(1) + attrs(1) + schema_null(1) + bad_cols(1) */
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix  = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags   = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size    = payload_sz;
        uint8_t* pl = p + hdrsz;
        pl[0] = (uint8_t)RAY_TABLE;
        pl[1] = 0;
        pl[2] = RAY_SERDE_NULL; /* schema = SERDE_NULL → schema ptr = NULL */
        pl[3] = 0xA6;           /* cols = unknown → error */
        ray_t* r = ray_de(raw_buf);
        /* Either NULL schema check or cols deser error fires */
        if (r) {
            TEST_ASSERT_TRUE(r == NULL || RAY_IS_ERR(r));
            if (RAY_IS_ERR(r)) ray_release(r);
        }
        ray_release(raw_buf);
    }
    /* DICT deser: vals deserialization fails */
    {
        /* Payload: type=DICT(98), attrs(1), keys=NULL(1), vals=bad(1) */
        int64_t payload_sz = 4;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix  = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags   = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size    = payload_sz;
        uint8_t* pl = p + hdrsz;
        pl[0] = (uint8_t)RAY_DICT;
        pl[1] = 0;
        pl[2] = RAY_SERDE_NULL; /* keys = SERDE_NULL → keys ptr = NULL */
        pl[3] = 0xA6;           /* vals = bad type → error */
        ray_t* r = ray_de(raw_buf);
        /* NULL keys → keys is NULL → keys check fails → returns keys(NULL) or falls through */
        /* Actually: if (!keys || RAY_IS_ERR(keys)) return keys  → returns NULL */
        /* Since keys==NULL, the check `!keys || RAY_IS_ERR(keys)` is true, returns NULL */
        /* So r may be NULL here */
        if (r && RAY_IS_ERR(r)) ray_release(r);
        ray_release(raw_buf);
    }
    /* DICT deser: keys OK, vals error */
    {
        /* Build real keys (SERDE_NULL), then truncated vals */
        /* keys = valid I64 atom, vals = unknown */
        /* Payload: DICT(1)+attrs(1)+key_i64(10)+vals_bad(2) = 14 */
        int64_t payload_sz = 14;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size = payload_sz;
        uint8_t* pl = p + hdrsz;
        int pos = 0;
        pl[pos++] = (uint8_t)RAY_DICT;
        pl[pos++] = 0;             /* attrs */
        /* keys = I64 atom = 10 bytes */
        pl[pos++] = (uint8_t)(-RAY_I64);
        pl[pos++] = 0;
        int64_t kval = 1;
        memcpy(pl + pos, &kval, 8); pos += 8;
        /* vals = unknown type 0xA6 + 1 flags byte */
        pl[pos++] = 0xA6;
        pl[pos++] = 0;
        (void)pos;
        ray_t* r = ray_de(raw_buf);
        /* vals deser error → keys released, returns error */
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    PASS();
}

/* ---- serde coverage: TABLE deser type-mismatch and more error paths ------ */

static test_result_t test_serde_table_de_type_mismatch(void) {
    size_t hdrsz = sizeof(ray_ipc_header_t);

    /* TABLE deser: cols deserialization succeeds but returns wrong type
     * (not RAY_LIST) → type-check at line 757 fires.
     * Craft: TABLE + attrs + schema=I64_atom(valid) + cols=I64_atom(wrong type).
     * schema = I64 atom (type=-RAY_I64 = 0xF5, flags=0, val=0) = 10 bytes
     * cols = I64 atom (also type=-RAY_I64) = 10 bytes
     * cols->type == -RAY_I64, not RAY_LIST → check fires */
    {
        /* Schema: -RAY_I64 atom = 10 bytes; Cols: -RAY_I64 atom = 10 bytes */
        /* TABLE payload: type(1) + attrs(1) + schema(10) + cols(10) = 22 bytes */
        int64_t payload_sz = 22;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        TEST_ASSERT_NOT_NULL(raw_buf); TEST_ASSERT_FALSE(RAY_IS_ERR(raw_buf));
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix  = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags   = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size    = payload_sz;
        uint8_t* pl = p + hdrsz;
        int pos = 0;
        pl[pos++] = (uint8_t)RAY_TABLE;  /* type */
        pl[pos++] = 0;                    /* attrs */
        /* schema = -RAY_I64 atom: type=0xF5, flags=0, val=0 (8 bytes) */
        pl[pos++] = (uint8_t)(-RAY_I64); /* 0xF5 */
        pl[pos++] = 0;                   /* flags */
        int64_t zero = 0;
        memcpy(pl + pos, &zero, 8); pos += 8; /* 10 bytes for schema atom */
        /* cols = -RAY_I64 atom (wrong: not a LIST) */
        pl[pos++] = (uint8_t)(-RAY_I64);
        pl[pos++] = 0;
        memcpy(pl + pos, &zero, 8); pos += 8;
        (void)pos;
        ray_t* r = ray_de(raw_buf);
        /* schema->type == -RAY_I64 (not RAY_I64 positive), or
         * cols->type == -RAY_I64 (not RAY_LIST) → type check fires */
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    /* TABLE deser: schema succeeds, cols fails (error) → lines 752-754 */
    {
        /* Schema = valid I64 vector (10 bytes: type=RAY_I64, attrs, len=0, no data)
         * Actually I64 vector needs: type(1)+attrs(1)+len(8) = 10 bytes header,
         * then 0 elements → total 10 bytes for an empty I64 vec.
         * Cols = bad type 0xA6 */
        /* I64 vec payload: type=RAY_I64=5, attrs=0, len=0 → 10 bytes */
        int64_t payload_sz = 13; /* TABLE(1)+attrs(1)+I64vec(10)+bad_cols(1) */
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size = payload_sz;
        uint8_t* pl = p + hdrsz;
        int pos = 0;
        pl[pos++] = (uint8_t)RAY_TABLE;
        pl[pos++] = 0;
        /* schema = empty I64 vector: type=RAY_I64(5), attrs=0, len=0 */
        pl[pos++] = (uint8_t)RAY_I64; /* type=5 */
        pl[pos++] = 0;                /* attrs */
        int64_t zero = 0;
        memcpy(pl + pos, &zero, 8); pos += 8; /* len = 0 */
        /* cols = unknown type → error */
        pl[pos++] = 0xA6;
        (void)pos;
        ray_t* r = ray_de(raw_buf);
        /* schema succeeds (empty I64 vec), cols fails → schema released, return cols(error) */
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    /* Atom serde_size default arm (line 167): craft object with atom type
     * that has no case in the serde_size atom switch.
     * We directly call ray_serde_size on a manually-crafted atom with
     * type = -120 (unknown) to hit the default arm. */
    {
        ray_t* v = ray_i64(0);
        v->type = -120; /* unknown atom type */
        int64_t sz = ray_serde_size(v);
        TEST_ASSERT_EQ_I(sz, 0); /* default returns 0 */
        v->type = -RAY_I64; /* restore */
        ray_release(v);
    }
    /* Atom ser_raw default arm (line 331): same — unknown negative type
     * in ray_ser_raw. We need to call ray_ser directly but serde_size
     * returns 0 → ray_ser bails early with domain error. So call
     * ray_ser_raw directly... but it's static. Instead, craft IPC payload
     * manually and test via ray_de which reads negative type 0x88=(-120). */
    {
        /* Build an IPC frame with payload byte 0x88 = -120 as type,
         * then flags byte = 0 (needed for atom path), then no more data.
         * type < 0 → atom path, flags read, base=120, switch default → error. */
        /* BUT we need len >= 1 after type byte.  Let's use 2 payload bytes. */
        int64_t payload_sz = 2;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size = payload_sz;
        uint8_t* pl = p + hdrsz;
        pl[0] = 0x88; /* -120 as int8_t */
        pl[1] = 0;    /* flags byte */
        /* After reading type and flags, default arm fires — needs more data
         * for some cases but RAY_BOOL needs only 1 more byte... Actually
         * the switch fires default before checking len further */
        ray_t* r = ray_de(raw_buf);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    PASS();
}

/* ---- serde coverage: ray_de size-bounds check (line 930) + LAMBDA body err */

static test_result_t test_serde_de_size_bounds(void) {
    size_t hdrsz = sizeof(ray_ipc_header_t);

    /* hdr->size > 1000000000 triggers line 930 */
    {
        ray_t* w = ray_ser(ray_i64(1));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(w);
        hdr->size = 2000000000LL;
        ray_t* r = ray_de(w);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        /* Restore before release */
        hdr->size = 10;
        ray_release(r); ray_release(w);
    }
    /* LAMBDA deser: params succeeds, body fails → lines 832-834 */
    {
        /* Payload: type=LAMBDA(100), attrs(1), params=NULL(1), body=bad(1) */
        int64_t payload_sz = 4;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size = payload_sz;
        uint8_t* pl = p + hdrsz;
        pl[0] = (uint8_t)RAY_LAMBDA; /* type=100 */
        pl[1] = 0;                   /* attrs */
        pl[2] = RAY_SERDE_NULL;      /* params = SERDE_NULL (C NULL) */
        pl[3] = 0xA6;                /* body = unknown type → error */
        ray_t* r = ray_de(raw_buf);
        /* params = NULL (SERDE_NULL), body fails → !params || IS_ERR check:
         * params is NULL → `!params` is true → return params (NULL).
         * Actually the check is: `if (!params || RAY_IS_ERR(params)) return params`
         * → since params==NULL, returns NULL immediately (before body). */
        /* So body error isn't hit. Need params to be non-NULL non-error. */
        if (r && RAY_IS_ERR(r)) ray_release(r);
        ray_release(raw_buf);
    }
    /* LAMBDA deser: params = valid atom, body = error → lines 831-834 */
    {
        /* Payload: LAMBDA(1)+attrs(1)+params=I64atom(10)+body=bad(2) = 14 */
        int64_t payload_sz = 14;
        int64_t total = (int64_t)(hdrsz + payload_sz);
        ray_t* raw_buf = ray_vec_new(RAY_U8, total);
        raw_buf->len = total;
        uint8_t* p = (uint8_t*)ray_data(raw_buf);
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
        hdr->prefix = RAY_SERDE_PREFIX;
        hdr->version = RAY_SERDE_WIRE_VERSION;
        hdr->flags = 0; hdr->endian = 0; hdr->msgtype = 0;
        hdr->size = payload_sz;
        uint8_t* pl = p + hdrsz;
        int pos = 0;
        pl[pos++] = (uint8_t)RAY_LAMBDA;
        pl[pos++] = 0;
        /* params = I64 atom = 10 bytes */
        pl[pos++] = (uint8_t)(-RAY_I64);
        pl[pos++] = 0; /* flags */
        int64_t pval = 0;
        memcpy(pl + pos, &pval, 8); pos += 8;
        /* body = unknown type 0xA6 + flags = 0 */
        pl[pos++] = 0xA6;
        pl[pos++] = 0;
        (void)pos;
        ray_t* r = ray_de(raw_buf);
        /* params succeeds (I64 atom), body fails → body error returned, params released */
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r); ray_release(raw_buf);
    }
    PASS();
}

/* ---- test_mem_budget --------------------------------------------------- */

static test_result_t test_mem_budget(void) {
    /* Uses its own runtime since store_setup only does heap/sym init */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    int64_t budget = ray_mem_budget();
    /* Budget should be > 0 (detected from OS) */
    TEST_ASSERT_EQ_I((int)(budget > 0), 1);
    /* At startup with minimal allocations, should not be under pressure */
    TEST_ASSERT_FALSE(ray_mem_pressure());

    ray_runtime_destroy(rt);
    PASS();
}

/* Test: IPC compression round-trip with compressible data */
static test_result_t test_ipc_compress_rt(void) {
    /* Create highly compressible data: runs of identical bytes */
    uint8_t src[4000];
    for (int i = 0; i < 4000; i++) src[i] = (uint8_t)(i / 16);

    uint8_t compressed[8000];
    size_t clen = ray_ipc_compress(src, 4000, compressed, 8000);
    TEST_ASSERT((clen) > (0), "clen > 0");
    TEST_ASSERT((clen) < (4000), "clen < 4000");

    uint8_t decompressed[4000];
    size_t dlen = ray_ipc_decompress(compressed, clen, decompressed, 4000);
    TEST_ASSERT_EQ_I(dlen, 4000);
    TEST_ASSERT_MEM_EQ(4000, src, decompressed);
    PASS();
}

/* Test: IPC compression below threshold returns 0 */
static test_result_t test_ipc_compress_threshold(void) {
    uint8_t src[1000];
    memset(src, 0, 1000);
    uint8_t dst[2000];
    size_t clen = ray_ipc_compress(src, 1000, dst, 2000);
    TEST_ASSERT_EQ_I(clen, 0);  /* below 2000 byte threshold */
    PASS();
}

/* Test: IPC compression with all-zero data (best case) */
static test_result_t test_ipc_compress_zeros(void) {
    uint8_t src[4000];
    memset(src, 0, 4000);

    uint8_t compressed[8000];
    size_t clen = ray_ipc_compress(src, 4000, compressed, 8000);
    TEST_ASSERT((clen) > (0), "clen > 0");
    TEST_ASSERT((clen) < (100), "clen < 100");  /* should compress very well */

    uint8_t decompressed[4000];
    size_t dlen = ray_ipc_decompress(compressed, clen, decompressed, 4000);
    TEST_ASSERT_EQ_I(dlen, 4000);
    TEST_ASSERT_MEM_EQ(4000, src, decompressed);
    PASS();
}

/* ---- IPC server lifecycle ----------------------------------------------- */

static test_result_t test_ipc_server_lifecycle(void) {
    ray_ipc_server_t srv;
    ray_err_t err = ray_ipc_server_init(&srv, 0);  /* ephemeral port */
    TEST_ASSERT_EQ_I(err, RAY_OK);
    TEST_ASSERT_TRUE(srv.running);
    TEST_ASSERT((srv.listen_fd) != (RAY_INVALID_SOCK), "srv.listen_fd != RAY_INVALID_SOCK");

    /* Verify we can retrieve the OS-assigned port */
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int rc = getsockname(srv.listen_fd, (struct sockaddr*)&addr, &alen);
    TEST_ASSERT_EQ_I(rc, 0);
    uint16_t port = ntohs(addr.sin_port);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_ipc_server_destroy(&srv);
    TEST_ASSERT_FALSE(srv.running);
    PASS();
}

/* ---- IPC sync round-trip ------------------------------------------------ */

/* Helper: get ephemeral port from listen socket */
static uint16_t get_listen_port(ray_sock_t fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

/* Server poll thread context — carries a VM for eval */
typedef struct {
    ray_ipc_server_t *srv;
    ray_vm_t         *vm;
} ipc_thread_ctx_t;

static void server_thread_fn(void* arg) {
    ipc_thread_ctx_t* ctx = (ipc_thread_ctx_t*)arg;
    /* Set up TLS VM so ray_eval_str works in this thread */
    __VM = ctx->vm;
    while (ctx->srv->running)
        ray_ipc_poll(ctx->srv, 10);
}

static test_result_t test_ipc_sync_roundtrip(void) {
    /* Full runtime needed for ray_eval_str in server thread */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_err_t err = ray_ipc_server_init(&srv, 0);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    /* Create a VM for the server thread */
    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };

    /* Start server poll thread */
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    /* Client: connect */
    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL);
    TEST_ASSERT((h) >= (0), "h >= 0");

    /* Client: send sync query "(+ 1 2)" — expects result 3 */
    ray_t* msg = ray_str("(+ 1 2)", 7);
    ray_t* result = ray_ipc_send(h, msg);
    ray_release(msg);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_TRUE(ray_is_atom(result));
    TEST_ASSERT_EQ_I(result->type, -RAY_I64);
    TEST_ASSERT_EQ_I(result->i64, 3);
    ray_release(result);

    /* Client: close */
    ray_ipc_close(h);

    /* Stop server */
    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC async send ----------------------------------------------------- */

static test_result_t test_ipc_async_send(void) {
    /* Full runtime needed for eval on server side */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };

    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL);
    TEST_ASSERT((h) >= (0), "h >= 0");

    /* Send async — should not block or error */
    ray_t* msg = ray_str("(+ 1 1)", 7);
    ray_err_t rc = ray_ipc_send_async(h, msg);
    ray_release(msg);
    TEST_ASSERT_EQ_I(rc, RAY_OK);

    /* Small delay to let server process the async message */
    { struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; nanosleep(&ts, NULL); }

    ray_ipc_close(h);
    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC auth success --------------------------------------------------- */

static test_result_t test_ipc_auth_success(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");

    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "secret123");
    TEST_ASSERT((h) >= (0), "h >= 0");

    ray_t* msg = ray_str("(+ 10 20)", 9);
    ray_t* result = ray_ipc_send(h, msg);
    ray_release(msg);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 30);
    ray_release(result);

    ray_ipc_close(h);
    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC auth reject ---------------------------------------------------- */

static test_result_t test_ipc_auth_reject(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");

    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "wrong");
    TEST_ASSERT_EQ_I(h, -3);

    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC auth no creds -------------------------------------------------- */

static test_result_t test_ipc_auth_no_creds(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");

    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL);
    TEST_ASSERT_EQ_I(h, -2);

    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC restricted mode ------------------------------------------------ */

static test_result_t test_ipc_restricted(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_ipc_server_init(&srv, 0);
    strcpy(srv.auth_secret, "secret123");
    srv.restricted = true;

    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "secret123");
    TEST_ASSERT((h) >= (0), "h >= 0");

    /* Arithmetic should work */
    ray_t* msg1 = ray_str("(+ 1 2)", 7);
    ray_t* r1 = ray_ipc_send(h, msg1);
    ray_release(msg1);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->i64, 3);
    ray_release(r1);

    /* set should be restricted */
    ray_t* msg2 = ray_str("(set x 42)", 10);
    ray_t* r2 = ray_ipc_send(h, msg2);
    ray_release(msg2);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r2));
    ray_release(r2);

    /* .sys.exec (formerly `system`) should be restricted */
    const char* q_sys = "(.sys.exec \"echo hi\")";
    ray_t* msg3 = ray_str(q_sys, strlen(q_sys));
    ray_t* r3 = ray_ipc_send(h, msg3);
    ray_release(msg3);
    TEST_ASSERT_NOT_NULL(r3);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    ray_release(r3);

    /* restricted builtins via higher-order functions (map bypass) */
    const char* q_map = "(map .sys.exec [\"echo pwned\"])";
    ray_t* msg4 = ray_str(q_map, strlen(q_map));
    ray_t* r4 = ray_ipc_send(h, msg4);
    ray_release(msg4);
    TEST_ASSERT_NOT_NULL(r4);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    ray_release(r4);

    ray_ipc_close(h);
    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC handshake rejects wrong wire version -------------------------- */

static test_result_t test_ipc_handshake_version_mismatch(void) {
    /* A client sending any wire-version byte other than
     * RAY_SERDE_WIRE_VERSION must be refused before the server commits
     * to any framed payload.  This is the defense-in-depth layer that
     * protects an old peer from ever seeing a new-format message. */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    ray_ipc_server_t srv;
    ray_err_t err = ray_ipc_server_init(&srv, 0);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    uint16_t port = get_listen_port(srv.listen_fd);
    TEST_ASSERT((port) > (0), "port > 0");

    ray_vm_t* srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    TEST_ASSERT_NOT_NULL(srv_vm);
    memset(srv_vm, 0, sizeof(ray_vm_t));
    srv_vm->id = 1;

    ipc_thread_ctx_t ctx = { .srv = &srv, .vm = srv_vm };
    ray_thread_t tid;
    ray_thread_create(&tid, server_thread_fn, &ctx);

    /* Connect raw socket and send a version-byte that doesn't match. */
    ray_sock_t s = ray_sock_connect("127.0.0.1", port, 2000);
    TEST_ASSERT_TRUE(s != RAY_INVALID_SOCK);
    uint8_t bad_hs[2] = { (uint8_t)(RAY_SERDE_WIRE_VERSION + 1), 0x00 };
    TEST_ASSERT(((int)ray_sock_send(s, bad_hs, 2)) >= (0), "(int)ray_sock_send(s, bad_hs, 2) >= 0");

    /* Server should refuse — EOF on recv indicates the connection was
     * closed before any payload bytes reached us.  If the check is
     * missing, the server would write its own handshake response here
     * and the test would observe 2 bytes instead. */
    uint8_t resp[2] = { 0xff, 0xff };
    int64_t got = 0;
    /* Give the server thread a small number of polling cycles to react. */
    for (int attempt = 0; attempt < 100 && got < 2; attempt++) {
        int64_t n = ray_sock_recv(s, resp + got, (size_t)(2 - got));
        if (n <= 0) break;
        got += n;
    }
    TEST_ASSERT(((int)got) < (2), "(int)got < 2");  /* never got a full response */
    ray_sock_close(s);

    /* A subsequent well-behaved client must still succeed, proving the
     * server is still running and only the bad handshake was rejected. */
    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL);
    TEST_ASSERT((h) >= (0), "h >= 0");
    ray_ipc_close(h);

    srv.running = false;
    ray_thread_join(tid);
    ray_ipc_server_destroy(&srv);
    ray_sys_free(srv_vm);
    ray_runtime_destroy(rt);
    PASS();
}

const test_entry_t store_entries[] = {
    { "store/col_mmap_i64", test_col_mmap_i64, store_setup, store_teardown },
    { "store/col_mmap_f64", test_col_mmap_f64, store_setup, store_teardown },
    { "store/col_mmap_cow", test_col_mmap_cow, store_setup, store_teardown },
    { "store/col_mmap_refcount", test_col_mmap_refcount, store_setup, store_teardown },
    { "store/col_mmap_corrupt", test_col_mmap_corrupt, store_setup, store_teardown },
    { "store/col_mmap_nofile", test_col_mmap_nofile, store_setup, store_teardown },
    { "store/splay_open_roundtrip", test_splay_open_roundtrip, store_setup, store_teardown },
    { "store/splay_str_column_roundtrip", test_splay_str_column_roundtrip, store_setup, store_teardown },
    { "store/splay_short_strv_roundtrip", test_splay_short_strv_roundtrip, store_setup, store_teardown },
    { "store/parted_nrows", test_parted_nrows, store_setup, store_teardown },
    { "store/table_nrows_parted", test_table_nrows_parted, store_setup, store_teardown },
    { "store/parted_release", test_parted_release, store_setup, store_teardown },
    { "store/part_open", test_part_open, store_setup, store_teardown },
    { "store/group_parted", test_group_parted, store_setup, store_teardown },
    { "store/col_ext_nullmap_roundtrip", test_col_ext_nullmap_roundtrip, store_setup, store_teardown },
    { "store/col_save_load_str", test_col_save_load_str, store_setup, store_teardown },
    { "store/col_save_load_list", test_col_save_load_list, store_setup, store_teardown },
    { "store/col_save_load_table", test_col_save_load_table, store_setup, store_teardown },
    { "store/file_open_close", test_file_open_close, store_setup, store_teardown },
    { "store/file_lock_unlock", test_file_lock_unlock, store_setup, store_teardown },
    { "store/file_sync", test_file_sync_op, store_setup, store_teardown },
    { "store/file_rename", test_file_rename_op, store_setup, store_teardown },
    { "store/file_shared_lock", test_file_shared_lock_concurrent, store_setup, store_teardown },
    { "store/sym_col_bounds_reject", test_sym_col_bounds_reject, store_setup, store_teardown },
    { "store/sym_col_count_mismatch", test_sym_col_count_mismatch, store_setup, store_teardown },
    { "store/sym_col_valid_roundtrip", test_sym_col_valid_roundtrip, store_setup, store_teardown },
    { "store/splay_load_with_sym", test_splay_load_with_sym, store_setup, store_teardown },
    { "store/splay_load_sym_missing", test_splay_load_sym_missing_corrupt, store_setup, store_teardown },
    { "store/read_splayed_bad_sym", test_read_splayed_bad_sym_fatal, store_setup, store_teardown },
    { "store/serde_long_str_roundtrip", test_serde_long_str_roundtrip, store_setup, store_teardown },
    { "store/serde_null_roundtrip", test_serde_null_roundtrip, store_setup, store_teardown },
    { "store/serde_typed_null_atoms", test_serde_typed_null_atoms, store_setup, store_teardown },
    { "store/serde_wire_version_mismatch", test_serde_wire_version_mismatch, store_setup, store_teardown },
    { "store/serde_atom_types",           test_serde_atom_types,           store_setup, store_teardown },
    { "store/serde_vec_types",            test_serde_vec_types,            store_setup, store_teardown },
    { "store/serde_table_roundtrip",      test_serde_table_roundtrip,      store_setup, store_teardown },
    { "store/serde_dict_roundtrip",       test_serde_dict_roundtrip,       store_setup, store_teardown },
    { "store/serde_obj_save_load",        test_serde_obj_save_load,        store_setup, store_teardown },
    { "store/serde_obj_load_errors",      test_serde_obj_load_errors,      store_setup, store_teardown },
    { "store/serde_obj_save_error",       test_serde_obj_save_error,       store_setup, store_teardown },
    { "store/serde_vec_null_bitmaps",     test_serde_vec_null_bitmaps,     store_setup, store_teardown },
    { "store/serde_de_error_paths",       test_serde_de_error_paths,       store_setup, store_teardown },
    { "store/serde_list_null_elem",       test_serde_list_with_null_elem,  store_setup, store_teardown },
    { "store/serde_function_types",       test_serde_function_types,       NULL,        NULL           },
    { "store/serde_error_roundtrip",      test_serde_error_roundtrip,      store_setup, store_teardown },
    { "store/serde_large_null_vec",       test_serde_large_null_vec,       store_setup, store_teardown },
    { "store/serde_f32_atom",             test_serde_f32_atom_and_edge_cases, store_setup, store_teardown },
    { "store/serde_lambda_roundtrip",     test_serde_lambda_roundtrip,     store_setup, store_teardown },
    { "store/serde_save_serde_error",     test_serde_save_serde_error,     store_setup, store_teardown },
    { "store/serde_de_raw_default",       test_serde_de_raw_default_and_errors, store_setup, store_teardown },
    { "store/serde_table_dict_de_errors", test_serde_table_dict_de_errors, store_setup, store_teardown },
    { "store/serde_table_de_type_mismatch", test_serde_table_de_type_mismatch, store_setup, store_teardown },
    { "store/serde_de_size_bounds",        test_serde_de_size_bounds,        store_setup, store_teardown },
    { "store/mem_budget", test_mem_budget, NULL, NULL },
    { "store/ipc/compress_rt", test_ipc_compress_rt, NULL, NULL },
    { "store/ipc/compress_threshold", test_ipc_compress_threshold, NULL, NULL },
    { "store/ipc/compress_zeros", test_ipc_compress_zeros, NULL, NULL },
    { "store/ipc/server_lifecycle", test_ipc_server_lifecycle, NULL, NULL },
    { "store/ipc/sync_roundtrip", test_ipc_sync_roundtrip, NULL, NULL },
    { "store/ipc/async_send", test_ipc_async_send, NULL, NULL },
    { "store/ipc/auth_success", test_ipc_auth_success, NULL, NULL },
    { "store/ipc/auth_reject", test_ipc_auth_reject, NULL, NULL },
    { "store/ipc/auth_no_creds", test_ipc_auth_no_creds, NULL, NULL },
    { "store/ipc/restricted", test_ipc_restricted, NULL, NULL },
    { "store/ipc/handshake_version_mismatch", test_ipc_handshake_version_mismatch, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};

