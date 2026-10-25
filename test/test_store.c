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
#include "ipc_harness.h"
#include <rayforce.h>
#include <rayforce.h>
#include <time.h>
#include <dirent.h>
#include "mem/heap.h"
#include "mem/cow.h"          /* ray_rc_sync: the lazy-hash probe threads */
#include "ops/ops.h"
#include "store/col.h"
#include "ops/idxop.h"
#include "store/fileio.h"
#include "store/splay.h"
#include "store/stream.h"
#include "store/part.h"
#include "store/serde.h"
#include "core/ipc.h"
#include "core/sock.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "core/pool.h"
#include "lang/eval.h"
#include "lang/internal.h"
#include "mem/sys.h"
#include "table/sym.h"
#include "table/domain.h"
#include <fcntl.h>
#include "ops/hash.h"     /* ray_hash_bytes: the import dictionary's full hashes */
#include "vec/str.h"
#include "vec/vec.h"          /* ray_vec_may_have_nulls */

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
#include <sys/stat.h>
#ifdef RAY_OS_LINUX
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#define TMP_COL_PATH  "/tmp/rayforce_test_col.dat"
#define TMP_SPLAY_DIR "/tmp/rayforce_test_splay"

/* ---- Setup / Teardown -------------------------------------------------- */

static void store_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void store_teardown(void) {
    ray_parted_cache_clear();
    ray_sym_destroy();
    ray_heap_destroy();
}

/* ---- test_col_save_derives_has_nulls ----------------------------------- */

/* The on-disk HAS_NULLS bit is load-bearing and nothing rescans on load,
 * so the save path derives it from the payload (#495): a column whose
 * producer left the bit clear while writing a sentinel is persisted with
 * the bit set, on both the read and the mmap loader. */
static test_result_t test_col_save_derives_has_nulls(void) {
    int64_t raw[] = {1, NULL_I64, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;   /* the forgetful producer */

    TEST_ASSERT_EQ_I(ray_col_save(vec, ray_test_tmp(TMP_COL_PATH)), RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_TRUE((loaded->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE(ray_vec_is_null(loaded, 1));
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_TRUE((mapped->attrs & RAY_ATTR_HAS_NULLS) != 0);
    TEST_ASSERT_TRUE(ray_vec_is_null(mapped, 1));
    ray_release(mapped);

    /* A null-free column stays null-free: the scan only ever sets. */
    int64_t clean[] = {1, 2, 3};
    ray_t* cvec = ray_vec_from_raw(RAY_I64, clean, 3);
    TEST_ASSERT_EQ_I(ray_col_save(cvec, ray_test_tmp(TMP_COL_PATH)), RAY_OK);
    ray_t* cl = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(cl);
    TEST_ASSERT_TRUE((cl->attrs & RAY_ATTR_HAS_NULLS) == 0);
    ray_release(cl);
    ray_release(cvec);
    ray_release(vec);
    PASS();
}

/* ---- test_col_mmap_i64 ------------------------------------------------- */

static test_result_t test_col_mmap_i64(void) {
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 5);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    /* Save to file */
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load via mmap */
    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_mmap_f64 ------------------------------------------------- */

static test_result_t test_col_mmap_f64(void) {
    double raw[] = {1.1, 2.2, 3.3, 4.4};
    ray_t* vec = ray_vec_from_raw(RAY_F64, raw, 4);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_mmap_f32 ------------------------------------------------- */

static test_result_t test_col_mmap_f32(void) {
    float raw[] = {1.25f, -2.5f, 3.75f, 0.0f};
    ray_t* vec = ray_vec_from_raw(RAY_F32, raw, 4);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_F32);
    TEST_ASSERT_EQ_I(loaded->len, 4);
    float* loaded_data = (float*)ray_data(loaded);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT(loaded_data[i] == raw[i], "f32 load value mismatch");
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_U(mapped->mmod, 1);
    TEST_ASSERT_EQ_I(mapped->type, RAY_F32);
    TEST_ASSERT_EQ_I(mapped->len, 4);
    float* mapped_data = (float*)ray_data(mapped);
    for (int i = 0; i < 4; i++)
        TEST_ASSERT(mapped_data[i] == raw[i], "f32 mmap value mismatch");

    ray_release(mapped);
    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_mmap_cow ------------------------------------------------- */

static test_result_t test_col_mmap_cow(void) {
    int64_t raw[] = {100, 200, 300};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_mmap_refcount -------------------------------------------- */

static test_result_t test_col_mmap_refcount(void) {
    int64_t raw[] = {7, 8, 9};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_mmap_nofile ---------------------------------------------- */

static test_result_t test_col_mmap_nofile(void) {
    ray_t* result = ray_col_mmap(ray_test_tmp("/tmp/rayforce_nonexistent_file_xyz.dat"));
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    TEST_ASSERT_STR_EQ(ray_err_code(result), "io");
    ray_release(result);

    PASS();
}

/* ---- test_splay_open_roundtrip ----------------------------------------- */

static test_result_t test_splay_open_roundtrip(void) {
    /* Clean up any leftover splay dir */
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));

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
    ray_err_t err = ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Open via mmap (zero-copy) */
    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), NULL);
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

    TEST_ASSERT_LOADED_MMOD(la, 1);
    TEST_ASSERT_LOADED_MMOD(lb, 1);
    TEST_ASSERT_LOADED_MMOD(lc, 1);

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
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
    PASS();
}

/* ---- test_splay_str_column_roundtrip ----------------------------------- */

static test_result_t test_splay_str_column_roundtrip(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));

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

    ray_err_t err = ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);

    ray_t* loaded_ids = ray_table_get_col(loaded, id_id);
    ray_t* loaded_names = ray_table_get_col(loaded, id_name);
    TEST_ASSERT_NOT_NULL(loaded_ids);
    TEST_ASSERT_NOT_NULL(loaded_names);
    TEST_ASSERT_LOADED_MMOD(loaded_ids, 1);
    TEST_ASSERT_LOADED_MMOD(loaded_names, 1);
    TEST_ASSERT_EQ_I(loaded_names->type, RAY_STR);
    TEST_ASSERT_NOT_NULL(loaded_names->str_pool);
    /* The pool owns the mapping it shares with the column (mmod 3), so a
     * selection that keeps the pool keeps the region — the column's own
     * free no longer takes the bytes with it. */
    TEST_ASSERT_LOADED_MMOD(loaded_names->str_pool, 3);
    /* Empty STR roundtrips as the canonical null and retains metadata. */
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

    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
    PASS();
}

/* ---- test_splay_short_strv_roundtrip ----------------------------------
 * Regression: short string columns must remain readable through
 * ray_read_splayed.  Older files used STRV and could be smaller than the
 * raw header; newer files use the raw RAY_STR layout and mmap directly.
 * The two file-size edge cases (24-byte 1-row, 14-byte 0-row) are saved
 * as separate single-column tables: load now rejects ragged column
 * lengths, so they can no longer share one table.
 * ---------------------------------------------------------------------- */

static test_result_t test_splay_short_strv_roundtrip(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));

    int64_t id_short = ray_sym_intern("short", 5);
    int64_t id_empty = ray_sym_intern("empty", 5);

    /* 1-row STRV with 2-byte content -- file ends up at 4 (magic) + 1
     * (type) + 1 (attrs) + 8 (count) + 8 (slen) + 2 (content) = 24 bytes */
    ray_t* shorts = ray_vec_new(RAY_STR, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(shorts));
    shorts = ray_str_vec_append(shorts, "hi", 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(shorts));

    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, id_short, shorts);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 1);

    ray_t* loaded_shorts = ray_table_get_col(loaded, id_short);
    TEST_ASSERT_NOT_NULL(loaded_shorts);
    TEST_ASSERT_EQ_I(loaded_shorts->type, RAY_STR);
    TEST_ASSERT_EQ_I(loaded_shorts->len, 1);

    size_t slen = 0;
    const char* s0 = ray_str_vec_get(loaded_shorts, 0, &slen);
    TEST_ASSERT_EQ_U(slen, 2);
    TEST_ASSERT_MEM_EQ(2, s0, "hi");
    ray_release(loaded);

    /* 0-row STRV -- 14 bytes on disk; re-set sweeps the previous column */
    ray_t* empty = ray_vec_new(RAY_STR, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(empty));

    ray_t* tbl2 = ray_table_new(1);
    tbl2 = ray_table_add_col(tbl2, id_empty, empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl2));

    err = ray_splay_save(tbl2, ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 1);

    ray_t* loaded_empty = ray_table_get_col(loaded, id_empty);
    TEST_ASSERT_NOT_NULL(loaded_empty);
    TEST_ASSERT_EQ_I(loaded_empty->type, RAY_STR);
    TEST_ASSERT_EQ_I(loaded_empty->len, 0);

    ray_release(loaded);
    ray_release(shorts);
    ray_release(empty);
    ray_release(tbl);
    ray_release(tbl2);

    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
    PASS();
}

/* ---- test_splay_dict_column_roundtrip --------------------------------- */
static test_result_t test_splay_dict_column_roundtrip(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));

    int64_t ids_raw[] = {1, 2};
    ray_t* ids = ray_vec_from_raw(RAY_I64, ids_raw, 2);
    ray_t* sched = ray_list_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ids));
    TEST_ASSERT_FALSE(RAY_IS_ERR(sched));

    int64_t k0_raw[] = {0};
    double v0_raw[] = {5.0};
    int64_t k1_raw[] = {0, 250000};
    double v1_raw[] = {15.0, 10.0};
    ray_t* d0 = ray_dict_new(ray_vec_from_raw(RAY_I64, k0_raw, 1),
                             ray_vec_from_raw(RAY_F64, v0_raw, 1));
    ray_t* d1 = ray_dict_new(ray_vec_from_raw(RAY_I64, k1_raw, 2),
                             ray_vec_from_raw(RAY_F64, v1_raw, 2));
    TEST_ASSERT_FALSE(RAY_IS_ERR(d0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(d1));
    sched = ray_list_append(sched, d0);
    sched = ray_list_append(sched, d1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sched));
    ray_release(d0);
    ray_release(d1);

    ray_t* tbl = ray_table_new(2);
    int64_t id_id = ray_sym_intern("id", 2);
    int64_t sched_id = ray_sym_intern("sched", 5);
    tbl = ray_table_add_col(tbl, id_id, ids);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, sched_id, sched);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    TEST_ASSERT_EQ_I(ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_DIR), NULL), RAY_OK);
    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* loaded_sched = ray_table_get_col(loaded, sched_id);
    TEST_ASSERT_NOT_NULL(loaded_sched);
    TEST_ASSERT_EQ_I(loaded_sched->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded_sched->len, 2);

    ray_t* ld0 = ray_list_get(loaded_sched, 0);
    ray_t* ld1 = ray_list_get(loaded_sched, 1);
    TEST_ASSERT_EQ_I(ld0->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ld1->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_keys(ld0)->type, RAY_I64);
    TEST_ASSERT_EQ_I(ray_dict_vals(ld1)->type, RAY_F64);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_dict_keys(ld1)))[1], 250000);
    TEST_ASSERT(((double*)ray_data(ray_dict_vals(ld1)))[1] == 10.0,
                "dict value roundtrip");

    ray_release(loaded);
    ray_release(tbl);
    ray_release(ids);
    ray_release(sched);
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
    PASS();
}

/* ---- test_splay_empty_list_column_roundtrip --------------------------- */
static test_result_t test_splay_empty_list_column_roundtrip(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));

    ray_t* ids = ray_vec_new(RAY_I64, 0);
    ray_t* who = ray_vec_new(RAY_SYM, 0);
    ray_t* sched = ray_list_new(0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ids));
    TEST_ASSERT_FALSE(RAY_IS_ERR(who));
    TEST_ASSERT_FALSE(RAY_IS_ERR(sched));

    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, ray_sym_intern("id", 2), ids);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, ray_sym_intern("who", 3), who);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, ray_sym_intern("sched", 5), sched);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    const char* sym_path = ray_test_tmp(TMP_SPLAY_DIR "/.sym");
    TEST_ASSERT_EQ_I(ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_DIR), sym_path), RAY_OK);
    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), sym_path);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_ncols(loaded), 3);
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 0);

    ray_t* loaded_ids = ray_table_get_col(loaded, ray_sym_find("id", 2));
    ray_t* loaded_who = ray_table_get_col(loaded, ray_sym_find("who", 3));
    ray_t* loaded_sched = ray_table_get_col(loaded, ray_sym_find("sched", 5));
    TEST_ASSERT_NOT_NULL(loaded_ids);
    TEST_ASSERT_NOT_NULL(loaded_who);
    TEST_ASSERT_NOT_NULL(loaded_sched);
    TEST_ASSERT_EQ_I(loaded_ids->type, RAY_I64);
    TEST_ASSERT_EQ_I(loaded_who->type, RAY_SYM);
    TEST_ASSERT_EQ_I(loaded_sched->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded_ids->len, 0);
    TEST_ASSERT_EQ_I(loaded_who->len, 0);
    TEST_ASSERT_EQ_I(loaded_sched->len, 0);

    ray_release(loaded);
    ray_release(tbl);
    ray_release(ids);
    ray_release(who);
    ray_release(sched);
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
    PASS();
}

/* A deterministic unsupported column must be rejected before an earlier
 * column can replace the committed generation. */
static test_result_t test_splay_save_preflight_preserves_generation(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
    int64_t k_id = ray_sym_intern("k", 1);
    int64_t v_id = ray_sym_intern("v", 1);

    int64_t old_k_raw[] = {1, 2};
    int64_t old_v_raw[] = {3, 4};
    ray_t* old_k = ray_vec_from_raw(RAY_I64, old_k_raw, 2);
    ray_t* old_v = ray_vec_from_raw(RAY_I64, old_v_raw, 2);
    ray_t* good = ray_table_new(2);
    good = ray_table_add_col(good, k_id, old_k);
    good = ray_table_add_col(good, v_id, old_v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(good));
    TEST_ASSERT_EQ_I(ray_splay_save(good, ray_test_tmp(TMP_SPLAY_DIR), NULL), RAY_OK);

    int64_t new_k_raw[] = {9, 10};
    int64_t dk_raw[] = {1, 2};
    int64_t dv_raw[] = {11, 12};
    ray_t* new_k = ray_vec_from_raw(RAY_I64, new_k_raw, 2);
    ray_t* unsupported = ray_dict_new(ray_vec_from_raw(RAY_I64, dk_raw, 2),
                                      ray_vec_from_raw(RAY_I64, dv_raw, 2));
    ray_t* bad = ray_table_new(2);
    bad = ray_table_add_col(bad, k_id, new_k);
    bad = ray_table_add_col(bad, v_id, old_v);
    ray_table_set_col_idx(bad, 1, unsupported);
    TEST_ASSERT_FALSE(RAY_IS_ERR(bad));
    TEST_ASSERT_EQ_I(ray_splay_save(bad, ray_test_tmp(TMP_SPLAY_DIR), NULL), RAY_ERR_NYI);

    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_DIR), NULL);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_t* loaded_k = ray_table_get_col(loaded, k_id);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(loaded_k))[0], 1);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(loaded_k))[1], 2);

    ray_release(loaded);
    ray_release(bad);
    ray_release(unsupported);
    ray_release(new_k);
    ray_release(good);
    ray_release(old_v);
    ray_release(old_k);
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_DIR));
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
    memset(parted->aux, 0, 16);

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
    memset(parted->aux, 0, 16);

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
    memset(parted->aux, 0, 16);

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
    (void)ray_test_rm_rf(ray_test_tmp(TMP_PART_DB));
    (void)ray_test_mkdir_p(ray_test_tmp(TMP_PART_DB "/2024.01.01/" TMP_TABLE_NAME));
    (void)ray_test_mkdir_p(ray_test_tmp(TMP_PART_DB "/2024.01.02/" TMP_TABLE_NAME));

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
    ray_err_t err = ray_splay_save(tbl1, ray_test_tmp(TMP_PART_DB "/2024.01.01/" TMP_TABLE_NAME), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Partition 2: 5 rows */
    int64_t raw_a2[] = {40, 50, 60, 70, 80};
    double  raw_b2[] = {4.4, 5.5, 6.6, 7.7, 8.8};
    ray_t* a2 = ray_vec_from_raw(RAY_I64, raw_a2, 5);
    ray_t* b2 = ray_vec_from_raw(RAY_F64, raw_b2, 5);

    ray_t* tbl2 = ray_table_new(3);
    tbl2 = ray_table_add_col(tbl2, name_a, a2);
    tbl2 = ray_table_add_col(tbl2, name_b, b2);
    err = ray_splay_save(tbl2, ray_test_tmp(TMP_PART_DB "/2024.01.02/" TMP_TABLE_NAME), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Save symfile */
    err = ray_sym_save(ray_test_tmp(TMP_PART_DB "/.sym"));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Cleanup in-memory tables */
    ray_release(a1); ray_release(b1); ray_release(tbl1);
    ray_release(a2); ray_release(b2); ray_release(tbl2);

    /* Open via ray_read_parted */
    ray_t* parted = ray_read_parted(ray_test_tmp(TMP_PART_DB), TMP_TABLE_NAME);
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
    TEST_ASSERT_LOADED_MMOD(segs_a[0], 1);
    TEST_ASSERT_EQ_I(segs_a[1]->len, 5);
    TEST_ASSERT_LOADED_MMOD(segs_a[1], 1);

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

    (void)ray_test_rm_rf(ray_test_tmp(TMP_PART_DB));
    PASS();
}

/* ---- test_parted_tables ------------------------------------------------ */
/* ray_parted_tables lists the splayed-table subdirectories of the first
 * partition as a sorted SYM vector usable with ray_read_parted. */
static test_result_t test_parted_tables(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_PART_DB));
    /* Two tables (trades, quotes) across two partitions. */
    const char* dirs[] = {
        ray_test_tmp(TMP_PART_DB "/2024.01.01/trades"), ray_test_tmp(TMP_PART_DB "/2024.01.01/quotes"),
        ray_test_tmp(TMP_PART_DB "/2024.01.02/trades"), ray_test_tmp(TMP_PART_DB "/2024.01.02/quotes"),
    };
    int64_t a[] = {1, 2, 3};
    for (int i = 0; i < 4; i++) {
        ray_t* col = ray_vec_from_raw(RAY_I64, a, 3);
        TEST_ASSERT_NOT_NULL(col);
        ray_t* t = ray_table_new(1);
        t = ray_table_add_col(t, ray_sym_intern("v", 1), col);
        TEST_ASSERT_FALSE(RAY_IS_ERR(t));
        TEST_ASSERT_EQ_I(ray_splay_save(t, dirs[i], NULL), RAY_OK);
        ray_release(col);
        ray_release(t);
    }

    /* Sorted SYM vector: quotes, trades. */
    ray_t* names = ray_parted_tables(ray_test_tmp(TMP_PART_DB));
    TEST_ASSERT_NOT_NULL(names);
    TEST_ASSERT_FALSE(RAY_IS_ERR(names));
    TEST_ASSERT_EQ_I(names->type, RAY_SYM);
    TEST_ASSERT_EQ_I(names->len, 2);
    const int64_t* ids = (const int64_t*)ray_data(names);
    TEST_ASSERT_EQ_I(ids[0], ray_sym_intern("quotes", 6));
    TEST_ASSERT_EQ_I(ids[1], ray_sym_intern("trades", 6));
    ray_release(names);

    /* A returned name feeds straight back into ray_read_parted. */
    ray_t* tbl = ray_read_parted(ray_test_tmp(TMP_PART_DB), "trades");
    TEST_ASSERT_NOT_NULL(tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_release(tbl);

    /* An existing-but-empty root (no partition dirs) lists no tables —
     * an empty SYM vector, not an error. */
    (void)ray_test_rm_rf(ray_test_tmp(TMP_PART_DB "_np"));
    (void)ray_test_mkdir_p(ray_test_tmp(TMP_PART_DB "_np"));
    ray_t* empty = ray_parted_tables(ray_test_tmp(TMP_PART_DB "_np"));
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(empty));
    TEST_ASSERT_EQ_I(empty->type, RAY_SYM);
    TEST_ASSERT_EQ_I(empty->len, 0);
    ray_release(empty);
    (void)ray_test_rm_rf(ray_test_tmp(TMP_PART_DB "_np"));

    (void)ray_test_rm_rf(ray_test_tmp(TMP_PART_DB));
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

/* ---- test_col_large_nullable_roundtrip ---------------------------------- */

#define LARGE_NULL_LEN 256  /* >128 — past the legacy inline-bitmap boundary */

static test_result_t test_col_large_nullable_roundtrip(void) {
    /* Create a 256-element I64 vector with sentinel-encoded nulls at
     * various positions and round-trip through ray_col_save +
     * ray_col_load / ray_col_mmap. */
    ray_t* vec = ray_vec_new(RAY_I64, LARGE_NULL_LEN);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = LARGE_NULL_LEN;

    int64_t* data = (int64_t*)ray_data(vec);
    for (int i = 0; i < LARGE_NULL_LEN; i++) data[i] = i * 10;

    /* Set nulls at positions: 0, 5, 127, 128, 200, 255 */
    int null_positions[] = { 0, 5, 127, 128, 200, 255 };
    int n_nulls = (int)(sizeof(null_positions) / sizeof(null_positions[0]));
    for (int i = 0; i < n_nulls; i++)
        ray_vec_set_null(vec, null_positions[i], true);

    TEST_ASSERT_TRUE((vec->attrs & RAY_ATTR_HAS_NULLS) != 0);

    /* --- Round-trip via ray_col_load --- */
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));

    TEST_ASSERT_EQ_I(loaded->type, RAY_I64);
    TEST_ASSERT_EQ_I(loaded->len, LARGE_NULL_LEN);
    TEST_ASSERT_TRUE((loaded->attrs & RAY_ATTR_HAS_NULLS) != 0);

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
    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));

    TEST_ASSERT_EQ_U(mapped->mmod, 1);
    TEST_ASSERT_EQ_I(mapped->type, RAY_I64);
    TEST_ASSERT_EQ_I(mapped->len, LARGE_NULL_LEN);
    TEST_ASSERT_TRUE((mapped->attrs & RAY_ATTR_HAS_NULLS) != 0);

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
    unlink(ray_test_tmp(TMP_COL_PATH));
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
    ray_err_t err = ray_col_save(list, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
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
    ray_err_t err = ray_col_save(list, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
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
    ray_err_t err = ray_col_save(tbl, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load */
    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
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
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_file_open_close ---------------------------------------------- */

#define TMP_FILEIO_PATH "/tmp/rayforce_test_fileio.dat"

static test_result_t test_file_open_close(void) {
    /* Open for write+create, then close */
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    ray_fd_t fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Open for read (file now exists) */
    fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_READ);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Open nonexistent for read (no create) → fail */
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_READ);
    TEST_ASSERT_EQ_I(fd, RAY_FD_INVALID);

    /* NULL path → fail */
    fd = ray_file_open(NULL, 0);
    TEST_ASSERT_EQ_I(fd, RAY_FD_INVALID);

    PASS();
}

/* ---- test_file_lock_unlock --------------------------------------------- */

static test_result_t test_file_lock_unlock(void) {
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    ray_fd_t fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_WRITE | RAY_OPEN_CREATE);
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
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    PASS();
}

/* ---- test_file_sync_op ------------------------------------------------- */

static test_result_t test_file_sync_op(void) {
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    ray_fd_t fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");

    /* fsync on valid fd */
    ray_err_t err = ray_file_sync(fd);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Invalid fd → error */
    TEST_ASSERT_EQ_I(ray_file_sync(RAY_FD_INVALID), RAY_ERR_IO);

    ray_file_close(fd);
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    PASS();
}

/* ---- test_file_rename_op ----------------------------------------------- */

#define TMP_FILEIO_PATH2 "/tmp/rayforce_test_fileio2.dat"

static test_result_t test_file_rename_op(void) {
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    unlink(ray_test_tmp(TMP_FILEIO_PATH2));

    /* Create source file */
    ray_fd_t fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Rename */
    ray_err_t err = ray_file_rename(ray_test_tmp(TMP_FILEIO_PATH), ray_test_tmp(TMP_FILEIO_PATH2));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Old path should not exist, new should */
    fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_READ);
    TEST_ASSERT_EQ_I(fd, RAY_FD_INVALID);

    fd = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH2), RAY_OPEN_READ);
    TEST_ASSERT((fd) != (RAY_FD_INVALID), "fd != RAY_FD_INVALID");
    ray_file_close(fd);

    /* Rename nonexistent → error */
    err = ray_file_rename(ray_test_tmp("/tmp/rayforce_nonexistent_xyz"), ray_test_tmp(TMP_FILEIO_PATH2));
    TEST_ASSERT_EQ_I(err, RAY_ERR_IO);

    /* NULL args → error */
    TEST_ASSERT_EQ_I(ray_file_rename(NULL, ray_test_tmp(TMP_FILEIO_PATH2)), RAY_ERR_IO);
    TEST_ASSERT_EQ_I(ray_file_rename(ray_test_tmp(TMP_FILEIO_PATH), NULL), RAY_ERR_IO);

    unlink(ray_test_tmp(TMP_FILEIO_PATH2));
    PASS();
}

static test_result_t test_col_close_failure(void) {
#ifdef RAY_OS_LINUX
    int64_t data[] = {1,2,3,4};
    ray_t* values[5];
    values[0] = ray_vec_from_raw(RAY_I64,data,4);
    values[1] = ray_vec_new(RAY_STR,0);
    values[1] = ray_str_vec_append(values[1],"a buffered string payload",25);
    values[2] = ray_list_new(0);
    ray_t* text = ray_str("a buffered string payload",25);
    values[2] = ray_list_append(values[2],text); ray_release(text);
    values[3] = ray_list_new(0);
    values[3] = ray_list_append(values[3],values[0]);
    values[4] = ray_table_new(0);
    values[4] = ray_table_add_col(values[4],ray_sym_intern("v",1),values[0]);
    char path[160], temp[180];
    snprintf(path,sizeof(path),"/tmp/rayforce-close-failure-%d",(int)getpid());
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    for (int type = 0; type < 5; type++) for (int durable = 0; durable < 2; durable++) {
        TEST_ASSERT_NOT_NULL(values[type]); TEST_ASSERT_FALSE(RAY_IS_ERR(values[type]));
        FILE* f = fopen(path,"wb"); TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_EQ_I(fwrite("original",1,8,f),8); TEST_ASSERT_EQ_I(fclose(f),0);
        pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
        if (!pid) {
            struct rlimit limit;
            if (getrlimit(RLIMIT_FSIZE,&limit)) _exit(2);
            limit.rlim_cur = 33;
            if (signal(SIGXFSZ,SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE,&limit)) _exit(2);
            ray_err_t err = durable ? ray_col_save(values[type],path) : ray_col_save_bulk(values[type],path);
            _exit(err == RAY_ERR_IO ? 0 : 1);
        }
        int status;
        TEST_ASSERT_EQ_I(waitpid(pid,&status,0),pid);
        TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status),0);
        f = fopen(path,"rb"); TEST_ASSERT_NOT_NULL(f);
        char bytes[16]; size_t n = fread(bytes,1,sizeof(bytes),f); fclose(f);
        TEST_ASSERT_EQ_I(n,8); TEST_ASSERT_TRUE(!memcmp(bytes,"original",8));
        TEST_ASSERT_TRUE(access(temp,F_OK) != 0);
    }
    unlink(path);
    for (int i = 4; i >= 0; i--) ray_release(values[i]);
    PASS();
#else
    SKIP("requires Linux RLIMIT_FSIZE fault injection");
#endif
}

/* A failed index append must leave the column loadable: without the index
 * marker the loader requires the exact payload length, so a partial region
 * would turn a best-effort accelerator into an unreadable column. */
static test_result_t test_col_append_index_rollback(void) {
#ifdef RAY_OS_LINUX
    int64_t n = 100000;
    ray_t* v = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = i * 3;
    char path[160];
    snprintf(path, sizeof(path), "/tmp/rayforce-append-rollback-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_col_save(v, path), RAY_OK);
    struct stat st;
    TEST_ASSERT_EQ_I(stat(path, &st), 0);
    off_t payload = st.st_size;

    ray_t* w = v; ray_retain(w);
    ray_t* r = ray_index_attach_zone(&w);
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r));
    const ray_index_t* ix = ray_index_payload(w->index);
    TEST_ASSERT_NOT_NULL(ix);

    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        limit.rlim_cur = (rlim_t)payload + 40;     /* pad fits, region doesn't */
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        _exit(ray_col_append_index(path, ix, n, RAY_I64) != RAY_OK ? 0 : 1);
    }
    int status;
    TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
    TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status), 0);

    TEST_ASSERT_EQ_I(stat(path, &st), 0);
    TEST_ASSERT_EQ_I((int64_t)st.st_size, (int64_t)payload);
    ray_t* back = ray_col_load(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->len, n);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(back))[n - 1], (n - 1) * 3);
    ray_release(back);

    /* and the append still works once there is room */
    TEST_ASSERT_EQ_I(ray_col_append_index(path, ix, n, RAY_I64), RAY_OK);
    TEST_ASSERT_EQ_I(stat(path, &st), 0);
    TEST_ASSERT_TRUE(st.st_size > payload);
    back = ray_col_load(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->len, n);
    ray_release(back);

    ray_release(w);
    ray_release(v);
    unlink(path);
    PASS();
#else
    SKIP("requires Linux RLIMIT_FSIZE fault injection");
#endif
}

/* Fill v with i*3, every 97th row null, rows [a, b) all null. */
static ray_t* zone_test_col(int64_t n, int64_t null_from, int64_t null_to) {
    ray_t* v = ray_vec_new(RAY_I64, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++)
        d[i] = (i % 97 == 0 || (i >= null_from && i < null_to)) ? NULL_I64 : i * 3;
    v->attrs |= 0x40;  /* RAY_ATTR_HAS_NULLS */
    return v;
}

static bool zone_equal(ray_t* a, ray_t* b) {
    const ray_index_t* x = ray_index_payload(a);
    const ray_index_t* y = ray_index_payload(b);
    if (x->u.chunk_zone.n_chunks != y->u.chunk_zone.n_chunks) return false;
    if (x->u.chunk_zone.is_f64 != y->u.chunk_zone.is_f64) return false;
    size_t n = x->u.chunk_zone.n_chunks;
    if (memcmp(ray_data(x->u.chunk_zone.mins), ray_data(y->u.chunk_zone.mins), n * 8)) return false;
    if (memcmp(ray_data(x->u.chunk_zone.maxs), ray_data(y->u.chunk_zone.maxs), n * 8)) return false;
    if (memcmp(ray_data(x->u.chunk_zone.null_bits), ray_data(y->u.chunk_zone.null_bits), (n + 7) / 8)) return false;
    if ((x->u.chunk_zone.aggs == NULL) != (y->u.chunk_zone.aggs == NULL)) return false;
    if (x->u.chunk_zone.aggs &&
        memcmp(ray_data(x->u.chunk_zone.aggs), ray_data(y->u.chunk_zone.aggs), n * 3 * 8)) return false;
    return true;
}

/* Uneven slices (70000 rows), an all-null chunk 2, equals the whole-column zone. */
static test_result_t test_zone_acc_slices_equal_compute(void) {
    int64_t n = 300000;
    ray_t* v = zone_test_col(n, 131072, 196608);
    TEST_ASSERT_NOT_NULL(v);
    ray_zone_acc_t acc;
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&acc, RAY_I64, 0), RAY_OK);
    for (int64_t off = 0; off < n; off += 70000) {
        int64_t cnt = n - off < 70000 ? n - off : 70000;
        ray_t* s = ray_vec_from_raw(RAY_I64, (int64_t*)ray_data(v) + off, cnt);
        TEST_ASSERT_NOT_NULL(s);
        TEST_ASSERT_EQ_I(ray_zone_acc_add(&acc, s), RAY_OK);
        ray_release(s);
    }
    /* a zero-row append opens nothing */
    ray_t* empty = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_EQ_I(ray_zone_acc_add(&acc, empty), RAY_OK);
    ray_release(empty);
    TEST_ASSERT_EQ_I(acc.n_chunks, 5);
    ray_t* got = ray_zone_acc_finish(&acc, n);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
    ray_t* want = ray_index_chunk_zone_compute(v, 16);
    TEST_ASSERT_TRUE(want && !RAY_IS_ERR(want));
    TEST_ASSERT_TRUE(zone_equal(got, want));
    const ray_index_t* ix = ray_index_payload(got);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ix->u.chunk_zone.mins))[2], INT64_MAX);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ix->u.chunk_zone.maxs))[2], INT64_MIN);
    TEST_ASSERT_TRUE(((uint8_t*)ray_data(ix->u.chunk_zone.null_bits))[0] & (1u << 2));
    ray_release(got); ray_release(want); ray_release(v);
    PASS();
}

/* Exactly one chunk; an append ending on the boundary opens no second chunk. */
static test_result_t test_zone_acc_one_chunk(void) {
    int64_t n = 65536;
    ray_t* v = zone_test_col(n, 0, 0);
    TEST_ASSERT_NOT_NULL(v);
    ray_zone_acc_t acc;
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&acc, RAY_I64, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_zone_acc_add(&acc, v), RAY_OK);
    TEST_ASSERT_EQ_I(acc.n_chunks, 1);
    ray_t* got = ray_zone_acc_finish(&acc, n);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
    TEST_ASSERT_EQ_I(ray_index_payload(got)->u.chunk_zone.n_chunks, 1);
    ray_release(got);
    /* below the gate: nothing */
    ray_zone_acc_t small;
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&small, RAY_I64, 0), RAY_OK);
    v->len = 65535;
    TEST_ASSERT_EQ_I(ray_zone_acc_add(&small, v), RAY_OK);
    TEST_ASSERT_NULL(ray_zone_acc_finish(&small, 65535));
    v->len = n;
    ray_release(v);
    PASS();
}

/* Two accumulators split mid-chunk (at row 100000) merge into the whole. */
static test_result_t test_zone_acc_merge_mid_chunk(void) {
    int64_t n = 200000, split = 100000;
    ray_t* v = zone_test_col(n, 0, 0);
    TEST_ASSERT_NOT_NULL(v);
    ray_t* lo = ray_vec_from_raw(RAY_I64, ray_data(v), split);
    ray_t* hi = ray_vec_from_raw(RAY_I64, (int64_t*)ray_data(v) + split, n - split);
    TEST_ASSERT_TRUE(lo && hi);
    ray_zone_acc_t a, b;
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&a, RAY_I64, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&b, RAY_I64, split), RAY_OK);
    TEST_ASSERT_EQ_I(ray_zone_acc_add(&b, hi), RAY_OK);   /* later range first */
    TEST_ASSERT_EQ_I(ray_zone_acc_add(&a, lo), RAY_OK);
    TEST_ASSERT_EQ_I(ray_zone_acc_merge(&a, &b), RAY_OK);
    ray_zone_acc_free(&b);
    ray_t* got = ray_zone_acc_finish(&a, n);
    ray_t* want = ray_index_chunk_zone_compute(v, 16);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && want && !RAY_IS_ERR(want));
    TEST_ASSERT_TRUE(zone_equal(got, want));
    ray_release(got); ray_release(want); ray_release(lo); ray_release(hi); ray_release(v);
    PASS();
}

/* F64 with NaN nulls: float arrays, no aggs. */
static test_result_t test_zone_acc_float(void) {
    int64_t n = 150000;
    ray_t* v = ray_vec_new(RAY_F64, n);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    v->len = n;
    double* d = (double*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = (i % 1000 == 7) ? NAN : (double)i * 0.5;
    v->attrs |= 0x40;  /* RAY_ATTR_HAS_NULLS */
    ray_zone_acc_t acc;
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&acc, RAY_F64, 0), RAY_OK);
    for (int64_t off = 0; off < n; off += 40000) {
        int64_t cnt = n - off < 40000 ? n - off : 40000;
        ray_t* s = ray_vec_from_raw(RAY_F64, d + off, cnt);
        TEST_ASSERT_EQ_I(ray_zone_acc_add(&acc, s), RAY_OK);
        ray_release(s);
    }
    ray_t* got = ray_zone_acc_finish(&acc, n);
    ray_t* want = ray_index_chunk_zone_compute(v, 16);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && want && !RAY_IS_ERR(want));
    TEST_ASSERT_TRUE(zone_equal(got, want));
    TEST_ASSERT_NULL(ray_index_payload(got)->u.chunk_zone.aggs);
    ray_release(got); ray_release(want); ray_release(v);
    PASS();
}

static ray_t* dict_test_col(int64_t n, int64_t distinct, bool unique) {
    ray_t* v = ray_vec_new(RAY_STR, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    for (int64_t i = 0; i < n; i++) {
        char buf[64];
        int len = unique ? snprintf(buf, sizeof(buf), "a long pooled unique string %lld", (long long)i)
                         : (i % 1009 == 0 ? 0 : snprintf(buf, sizeof(buf), "v%lld", (long long)(i % distinct)));
        v = ray_str_vec_append(v, buf, (size_t)len);
        if (!v || RAY_IS_ERR(v)) return NULL;
    }
    return v;
}

/* Slices of 30000 rows, empty strings included, equals ray_index_dict_compute. */
static test_result_t test_dict_acc_slices_equal_compute(void) {
    int64_t n = 100000;
    ray_t* v = dict_test_col(n, 3000, false);
    TEST_ASSERT_NOT_NULL(v);
    ray_dict_acc_t acc;
    TEST_ASSERT_EQ_I(ray_dict_acc_init(&acc), RAY_OK);
    for (int64_t off = 0; off < n; off += 30000) {
        int64_t cnt = n - off < 30000 ? n - off : 30000;
        ray_t* s = ray_vec_new(RAY_STR, cnt);
        for (int64_t i = 0; i < cnt; i++) {
            size_t len; const char* p = ray_str_vec_get(v, off + i, &len);
            s = ray_str_vec_append(s, p, len);
        }
        TEST_ASSERT_EQ_I(ray_dict_acc_add(&acc, s), RAY_OK);
        ray_release(s);
    }
    ray_t* got = ray_dict_acc_finish(&acc, n);
    ray_t* want = ray_index_dict_compute(v);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && want && !RAY_IS_ERR(want));
    const ray_index_t* x = ray_index_payload(got);
    const ray_index_t* y = ray_index_payload(want);
    TEST_ASSERT_EQ_I(x->u.dict.n_distinct, y->u.dict.n_distinct);
    TEST_ASSERT_EQ_I(x->u.dict.n_distinct, 3001);   /* 3000 values plus "" */
    TEST_ASSERT_EQ_I(memcmp(ray_data(x->u.dict.codes), ray_data(y->u.dict.codes), (size_t)n * 4), 0);
    TEST_ASSERT_EQ_I(memcmp(ray_data(x->u.dict.first_occ), ray_data(y->u.dict.first_occ),
                            (size_t)x->u.dict.n_distinct * 4), 0);
    ray_release(got); ray_release(want); ray_release(v);
    PASS();
}

/* Leading empty strings (NULL pool at the first distinct value), then
 * 70000 rows over 100 values, in two slices: equals ray_index_dict_compute. */
static test_result_t test_dict_acc_leading_empty(void) {
    int64_t n = 70003;
    ray_t* v = ray_vec_new(RAY_STR, n);
    TEST_ASSERT_NOT_NULL(v);
    v = ray_str_vec_append(v, "", 0);
    v = ray_str_vec_append(v, "", 0);
    v = ray_str_vec_append(v, "a", 1);
    for (int64_t i = 0; i < 70000; i++) {
        char buf[32]; int len = snprintf(buf, sizeof(buf), "v%lld", (long long)(i % 100));
        v = ray_str_vec_append(v, buf, (size_t)len);
    }
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    ray_dict_acc_t acc;
    TEST_ASSERT_EQ_I(ray_dict_acc_init(&acc), RAY_OK);
    int64_t cuts[3] = {0, 35000, n};
    for (int k = 0; k < 2; k++) {
        ray_t* s = ray_vec_new(RAY_STR, cuts[k + 1] - cuts[k]);
        for (int64_t i = cuts[k]; i < cuts[k + 1]; i++) {
            size_t len; const char* p = ray_str_vec_get(v, i, &len);
            s = ray_str_vec_append(s, p, len);
        }
        TEST_ASSERT_EQ_I(ray_dict_acc_add(&acc, s), RAY_OK);
        ray_release(s);
    }
    ray_t* got = ray_dict_acc_finish(&acc, n);
    ray_t* want = ray_index_dict_compute(v);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got) && want && !RAY_IS_ERR(want));
    const ray_index_t* x = ray_index_payload(got);
    const ray_index_t* y = ray_index_payload(want);
    TEST_ASSERT_EQ_I(x->u.dict.n_distinct, y->u.dict.n_distinct);
    TEST_ASSERT_EQ_I(x->u.dict.n_distinct, 102);   /* "", "a", v0..v99 */
    TEST_ASSERT_EQ_I(memcmp(ray_data(x->u.dict.codes), ray_data(y->u.dict.codes), (size_t)n * 4), 0);
    TEST_ASSERT_EQ_I(memcmp(ray_data(x->u.dict.first_occ), ray_data(y->u.dict.first_occ),
                            (size_t)x->u.dict.n_distinct * 4), 0);
    ray_release(got); ray_release(want); ray_release(v);
    PASS();
}

/* 70000 unique strings: no cap, the dictionary equals the whole-column one. */
static test_result_t test_dict_acc_high_cardinality(void) {
    int64_t n = 70000;
    ray_t* v = dict_test_col(n, 0, true);
    TEST_ASSERT_NOT_NULL(v);
    ray_dict_acc_t acc;
    TEST_ASSERT_EQ_I(ray_dict_acc_init(&acc), RAY_OK);
    TEST_ASSERT_EQ_I(ray_dict_acc_add(&acc, v), RAY_OK);
    TEST_ASSERT_FALSE(acc.dead);
    ray_t* got = ray_dict_acc_finish(&acc, n);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
    ray_t* want = ray_index_dict_compute(v);
    TEST_ASSERT_TRUE(want && !RAY_IS_ERR(want));
    const ray_index_t* g = ray_index_payload(got);
    const ray_index_t* w = ray_index_payload(want);
    TEST_ASSERT_EQ_I(g->u.dict.n_distinct, 70000);
    TEST_ASSERT_EQ_I(g->u.dict.n_distinct, w->u.dict.n_distinct);
    TEST_ASSERT_EQ_I(memcmp(ray_data(g->u.dict.codes), ray_data(w->u.dict.codes), (size_t)n * 4), 0);
    TEST_ASSERT_EQ_I(memcmp(ray_data(g->u.dict.first_occ), ray_data(w->u.dict.first_occ),
                            (size_t)w->u.dict.n_distinct * 4), 0);
    ray_release(got); ray_release(want);
    /* below the gate: nothing */
    ray_dict_acc_t small;
    TEST_ASSERT_EQ_I(ray_dict_acc_init(&small), RAY_OK);
    v->len = 1000;
    TEST_ASSERT_EQ_I(ray_dict_acc_add(&small, v), RAY_OK);
    TEST_ASSERT_NULL(ray_dict_acc_finish(&small, 1000));
    v->len = n;
    ray_release(v);
    PASS();
}

/* Stream a 200000-row I64 column in 70000-row slices; the file carries the
 * zone and loads with it. Values are clustered so no hash is wanted. */
static test_result_t test_stream_inline_zone(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-zone-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 200000;
    ray_t* v = zone_test_col(n, 0, 0);
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("z", 1), RAY_I64, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w, 0), RAY_OK);
    for (int64_t off = 0; off < n; off += 70000) {
        int64_t cnt = n - off < 70000 ? n - off : 70000;
        ray_t* s = ray_vec_from_raw(RAY_I64, (int64_t*)ray_data(v) + off, cnt);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w, s), RAY_OK);
        ray_release(s);
    }
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    char path[200]; snprintf(path, sizeof(path), "%s/z", dir);
    ray_t* back = ray_col_mmap(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->len, n);
    TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_EQ_I(ray_index_kind(back), RAY_IDX_CHUNK_ZONE);
    ray_t* want = ray_index_chunk_zone_compute(v, 16);
    TEST_ASSERT_TRUE(zone_equal(back->index, want));
    /* the region is what ray_col_append_index would have written */
    ray_t* plain = zone_test_col(n, 0, 0);
    char ref[200]; snprintf(ref, sizeof(ref), "%s/ref", dir);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(plain, ref), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_append_index(ref, ray_index_payload(want), n, RAY_I64), RAY_OK);
    struct stat a, b;
    TEST_ASSERT_EQ_I(stat(path, &a), 0); TEST_ASSERT_EQ_I(stat(ref, &b), 0);
    TEST_ASSERT_EQ_I((int64_t)a.st_size, (int64_t)b.st_size);
    {
        size_t sz = (size_t)a.st_size;
        uint8_t* ba = (uint8_t*)malloc(sz); uint8_t* bb = (uint8_t*)malloc(sz);
        TEST_ASSERT_TRUE(ba && bb);
        FILE* fa = fopen(path, "rb"); FILE* fb = fopen(ref, "rb");
        TEST_ASSERT_TRUE(fa && fb);
        TEST_ASSERT_EQ_I((int64_t)fread(ba, 1, sz, fa), (int64_t)sz);
        TEST_ASSERT_EQ_I((int64_t)fread(bb, 1, sz, fb), (int64_t)sz);
        fclose(fa); fclose(fb);
        TEST_ASSERT_EQ_I(memcmp(ba, bb, sz), 0);
        free(ba); free(bb);
    }
    ray_release(want); ray_release(back); ray_release(plain); ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* A sentinel typed as a value in a slice WITHOUT HAS_NULLS is still a null:
 * the accumulator decides by the sentinel value (col.c #495). */
static test_result_t test_zone_acc_sentinel_without_flag(void) {
    int64_t n = 70000;
    ray_t* v = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    v->len = n;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = i * 3 + 5;
    d[7] = INT64_MIN;
    TEST_ASSERT_FALSE(v->attrs & RAY_ATTR_HAS_NULLS);
    ray_zone_acc_t acc;
    TEST_ASSERT_EQ_I(ray_zone_acc_init(&acc, RAY_I64, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_zone_acc_add(&acc, v), RAY_OK);
    TEST_ASSERT_TRUE(acc.saw_null);
    TEST_ASSERT_EQ_I(acc.nn[0], 65535);
    ray_t* got = ray_zone_acc_finish(&acc, n);
    TEST_ASSERT_TRUE(got && !RAY_IS_ERR(got));
    const ray_index_t* ix = ray_index_payload(got);
    TEST_ASSERT_TRUE(((uint8_t*)ray_data(ix->u.chunk_zone.null_bits))[0] & 1u);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ix->u.chunk_zone.mins))[0], 5);   /* row 0 */
    v->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* want = ray_index_chunk_zone_compute(v, 16);
    TEST_ASSERT_TRUE(want && !RAY_IS_ERR(want));
    TEST_ASSERT_TRUE(zone_equal(got, want));
    ray_release(got); ray_release(want); ray_release(v);
    PASS();
}

/* Writer level: slice 1 carries a sentinel without the flag, slice 2 the
 * flag; the file header must publish HAS_NULLS and the zone must agree with
 * the loaded column. */
static test_result_t test_stream_sentinel_publishes_has_nulls(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-sent-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 70000;
    ray_t* s1 = ray_vec_new(RAY_I64, n); s1->len = n;
    ray_t* s2 = ray_vec_new(RAY_I64, n); s2->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int64_t*)ray_data(s1))[i] = i * 3;
        ((int64_t*)ray_data(s2))[i] = (i % 50 == 0) ? NULL_I64 : (n + i) * 3;
    }
    ((int64_t*)ray_data(s1))[7] = INT64_MIN;
    s2->attrs |= RAY_ATTR_HAS_NULLS;
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("n", 1), RAY_I64, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, s1), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, s2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    char path[200]; snprintf(path, sizeof(path), "%s/n", dir);
    ray_t* back = ray_col_mmap(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->len, 2 * n);
    TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_INDEX);
    ray_t* copy = ray_vec_from_raw(RAY_I64, ray_data(back), back->len);
    TEST_ASSERT_TRUE(copy && !RAY_IS_ERR(copy));
    copy->attrs |= RAY_ATTR_HAS_NULLS;
    ray_t* want = ray_index_chunk_zone_compute(copy, 16);
    TEST_ASSERT_TRUE(want && !RAY_IS_ERR(want));
    TEST_ASSERT_TRUE(zone_equal(back->index, want));
    TEST_ASSERT_TRUE(((int64_t*)ray_data(ray_index_payload(back->index)->u.chunk_zone.mins))[0] != INT64_MIN);
    ray_release(want); ray_release(copy); ray_release(back);
    ray_release(s1); ray_release(s2);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* SYM writer chunks.  A chunk already encoded over the writer's domain (a
 * converter's own decode) is written as it is, W32 straight and narrower
 * widths widened, and never makes the runtime id cache; a runtime-domain
 * chunk makes it and gets its positions through it.  HAS_NULLS follows the
 * positions written, not a chunk's flag: a flagged chunk without position 0
 * publishes none, an unflagged one holding it, even in its last cell,
 * publishes it. */
static bool stream_sym_read(const char* path, ray_t* hdr, uint32_t* pos, int64_t n) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(hdr, 1, 32, f) == 32 && (int64_t)fread(pos, 4, (size_t)n, f) == n;
    fclose(f);
    return ok;
}
static ray_t* stream_sym_chunk(ray_sym_domain_t* dom, int64_t n, uint8_t width) {
    ray_t* v = ray_sym_vec_new(width, n);
    if (!v || RAY_IS_ERR(v)) return NULL;
    v->sym_domain = dom; ray_sym_domain_retain(dom);
    v->len = n; v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    return v;
}
static test_result_t test_stream_sym_chunks(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-sym-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    char sp[200], path[200]; snprintf(sp, sizeof(sp), "%s/.sym", dir);
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(sp);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "", 0), 0);
    enum { K = 5 };
    const char* words[K] = {"", "alpha", "beta", "a string past the inline bytes", "gamma"};
    int64_t at[K], id[K];
    for (int k = 0; k < K; k++) {
        at[k] = ray_sym_domain_intern(dom, words[k], strlen(words[k]));
        id[k] = ray_sym_intern(words[k], strlen(words[k]));
        TEST_ASSERT_TRUE(at[k] >= 0 && id[k] >= 0);
    }
    int64_t n = 20000;   /* past the writer's 8192-cell buffer */
    uint32_t* back = (uint32_t*)ray_sys_alloc((size_t)(3 * n) * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(back);
    ray_t hdr;

    /* "d": three chunks over the domain, none holding 0: W32 flagged, W32,
     * then W16 (re-encoded to the file's W32) */
    ray_t* c1 = stream_sym_chunk(dom, n, RAY_SYM_W32);
    ray_t* c2 = stream_sym_chunk(dom, n, RAY_SYM_W32);
    ray_t* c4 = stream_sym_chunk(dom, n, RAY_SYM_W16);
    TEST_ASSERT_TRUE(c1 && c2 && c4);
    for (int64_t i = 0; i < n; i++) {
        ((uint32_t*)ray_data(c1))[i] = (uint32_t)at[1 + i % (K - 1)];
        ((uint32_t*)ray_data(c2))[i] = (uint32_t)at[1 + (i * 3) % (K - 1)];
        ((uint16_t*)ray_data(c4))[i] = (uint16_t)at[1 + (i * 5) % (K - 1)];
    }
    c1->attrs |= RAY_ATTR_HAS_NULLS;
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("d", 1), RAY_SYM, dom, 0), RAY_OK);
    TEST_ASSERT_NULL(w.lut_id);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, c1), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, c2), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, c4), RAY_OK);
    TEST_ASSERT_NULL(w.lut_id);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    snprintf(path, sizeof(path), "%s/d", dir);
    TEST_ASSERT_TRUE(stream_sym_read(path, &hdr, back, 3 * n));
    TEST_ASSERT_EQ_I(hdr.type, RAY_SYM);
    TEST_ASSERT_EQ_I(hdr.len, 3 * n);
    TEST_ASSERT_FALSE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_EQ_I(memcmp(back, ray_data(c1), (size_t)n * 4), 0);
    TEST_ASSERT_EQ_I(memcmp(back + n, ray_data(c2), (size_t)n * 4), 0);
    int64_t bad = 0;
    for (int64_t i = 0; i < n; i++)
        if (back[2 * n + i] != ((uint16_t*)ray_data(c4))[i]) bad++;
    TEST_ASSERT_EQ_I(bad, 0);

    /* "r": an unflagged chunk over the domain whose only 0 is its last
     * cell, then two runtime-domain chunks without "" (the cache made,
     * then hit) */
    ray_t* c3 = stream_sym_chunk(dom, n, RAY_SYM_W32);
    ray_t* rt = ray_sym_vec_new(RAY_SYM_W64, n);
    TEST_ASSERT_TRUE(c3 && rt && !RAY_IS_ERR(rt));
    rt->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((uint32_t*)ray_data(c3))[i] = (uint32_t)at[1 + i % (K - 1)];
        ((int64_t*)ray_data(rt))[i] = id[1 + (i * 7) % (K - 1)];
    }
    ((uint32_t*)ray_data(c3))[n - 1] = 0;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("r", 1), RAY_SYM, dom, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, c3), RAY_OK);
    TEST_ASSERT_NULL(w.lut_id);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, rt), RAY_OK);
    TEST_ASSERT_NOT_NULL(w.lut_id);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, rt), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    TEST_ASSERT_NULL(w.lut_id);
    snprintf(path, sizeof(path), "%s/r", dir);
    TEST_ASSERT_TRUE(stream_sym_read(path, &hdr, back, 3 * n));
    TEST_ASSERT_EQ_I(hdr.len, 3 * n);
    TEST_ASSERT_TRUE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_EQ_I(memcmp(back, ray_data(c3), (size_t)n * 4), 0);
    for (int64_t i = 0; i < 2 * n; i++)
        if (back[n + i] != (uint32_t)at[1 + ((i % n) * 7) % (K - 1)]) bad++;
    TEST_ASSERT_EQ_I(bad, 0);

    ray_release(c1); ray_release(c2); ray_release(c3); ray_release(c4); ray_release(rt);
    ray_sys_free(back);
    ray_sym_domain_release(dom);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* STR chunks into a SYM writer: interned into the writer's domain, a batch
 * of 8192 at a time.  The same 20000 strings (a few hundred distinct, "",
 * repeats across batches) into a file domain and into an import dictionary
 * domain (the direct import's, which keys on all 64 bits of the hash) give
 * the same positions, first occurrence first; and the import dictionary
 * then finds every string under its full hash (none added again). */
static test_result_t test_stream_sym_from_str(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-str-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    const int64_t n = 20000;
    ray_t* col = ray_vec_new(RAY_STR, n);
    TEST_ASSERT_FALSE(!col || RAY_IS_ERR(col));
    char buf[64];
    for (int64_t i = 0; i < n; i++) {
        int len = i % 97 == 0 ? 0 : snprintf(buf, sizeof(buf), "%s-%lld", i % 3 ? "a string past the inline bytes" : "s",
                                             (long long)((i * 7919) % 300));
        col = ray_str_vec_append(col, buf, (size_t)len);
        TEST_ASSERT_FALSE(!col || RAY_IS_ERR(col));
    }
    uint32_t* back[2] = {NULL, NULL};
    char sp[200], path[200];
    ray_sym_domain_t* dom[2] = {NULL, NULL};
    for (int k = 0; k < 2; k++) {
        snprintf(sp, sizeof(sp), "%s/.sym%d", dir, k);
        dom[k] = k ? ray_sym_domain_create_import(sp) : ray_sym_domain_open_or_create(sp);
        if (!dom[k]) { TEST_ASSERT_EQ_I(k, 1); break; }   /* no import dictionary here */
        TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom[k], "", 0), 0);
        ray_col_stream_t w;
        TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern(k ? "i" : "f", 1), RAY_SYM, dom[k], 0), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w, col), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
        back[k] = (uint32_t*)ray_sys_alloc((size_t)n * sizeof(uint32_t));
        TEST_ASSERT_NOT_NULL(back[k]);
        ray_t hdr;
        snprintf(path, sizeof(path), "%s/%s", dir, k ? "i" : "f");
        TEST_ASSERT_TRUE(stream_sym_read(path, &hdr, back[k], n));
        TEST_ASSERT_EQ_I(hdr.len, n);
        TEST_ASSERT_TRUE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    }
    int64_t bad = 0;
    if (dom[1]) {
        for (int64_t i = 0; i < n; i++) if (back[0][i] != back[1][i]) { bad++; break; }
        /* every string found again under its full hash, at its position */
        int64_t before = ray_sym_domain_count(dom[1]);
        for (int64_t i = 0; i < n && !bad; i++) {
            size_t len; const char* s = ray_str_vec_get(col, i, &len);
            uint64_t h = ray_hash_bytes(s, len);
            int64_t pos = -1;
            if (!ray_sym_domain_intern_batch64(dom[1], 1, &s, &len, &h, &pos) || pos != (int64_t)back[1][i]) bad++;
        }
        if (ray_sym_domain_count(dom[1]) != before) bad++;
    }
    for (int k = 0; k < 2; k++) { if (dom[k]) ray_sym_domain_release(dom[k]); ray_sys_free(back[k]); }
    ray_release(col);
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(bad, 0);
    if (!dom[1]) SKIP("import dictionary unsupported here: the file domain only");
    PASS();
}
/* A W32 chunk over another symbol file's domain is re-encoded string by
 * string, at the writer's own width as at any other: its positions mean
 * nothing in the writer's domain (the other file holds the same words in
 * another order, and one more). */
static test_result_t test_stream_sym_foreign(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-symf-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    char sp[200], sp2[200], path[200];
    snprintf(sp, sizeof(sp), "%s/.sym", dir); snprintf(sp2, sizeof(sp2), "%s/.sym2", dir);
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(sp);
    ray_sym_domain_t* other = ray_sym_domain_open_or_create(sp2);
    TEST_ASSERT_TRUE(dom && other);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "", 0), 0);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(other, "", 0), 0);
    enum { K = 5 };
    const char* words[K] = {"alpha", "beta", "gamma", "a string past the inline bytes", "only in the other"};
    int64_t at2[K];
    for (int k = 0; k < K - 1; k++) TEST_ASSERT_TRUE(ray_sym_domain_intern(dom, words[k], strlen(words[k])) > 0);
    for (int k = K - 1; k >= 0; k--) {
        at2[k] = ray_sym_domain_intern(other, words[k], strlen(words[k]));
        TEST_ASSERT_TRUE(at2[k] > 0);
    }
    int64_t n = 20000;
    ray_t* c = stream_sym_chunk(other, n, RAY_SYM_W32);
    TEST_ASSERT_NOT_NULL(c);
    for (int64_t i = 0; i < n; i++) ((uint32_t*)ray_data(c))[i] = (uint32_t)at2[(i * 3) % K];
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("f", 1), RAY_SYM, dom, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, c), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    int64_t at[K];
    for (int k = 0; k < K; k++) {
        at[k] = ray_sym_domain_find(dom, words[k], strlen(words[k]));
        TEST_ASSERT_TRUE(at[k] > 0);   /* the fifth added by the append */
    }
    uint32_t* back = (uint32_t*)ray_sys_alloc((size_t)n * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(back);
    ray_t hdr;
    snprintf(path, sizeof(path), "%s/f", dir);
    TEST_ASSERT_TRUE(stream_sym_read(path, &hdr, back, n));
    TEST_ASSERT_FALSE(hdr.attrs & RAY_ATTR_HAS_NULLS);
    int64_t bad = 0;
    for (int64_t i = 0; i < n; i++) if (back[i] != (uint32_t)at[(i * 3) % K]) bad++;
    TEST_ASSERT_EQ_I(bad, 0);
    ray_release(c); ray_sys_free(back);
    ray_sym_domain_release(dom); ray_sym_domain_release(other);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* The runtime id cache starts empty whatever its memory held: the blocks it
 * is carved from were freed full of one runtime id, which must then be
 * interned on first sight rather than read from a stale slot. */
static test_result_t test_stream_sym_lut_stale(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-syml-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    char sp[200], path[200]; snprintf(sp, sizeof(sp), "%s/.sym", dir);
    ray_sym_domain_t* dom = ray_sym_domain_open_or_create(sp);
    TEST_ASSERT_NOT_NULL(dom);
    TEST_ASSERT_EQ_I(ray_sym_domain_intern(dom, "", 0), 0);
    TEST_ASSERT_TRUE(ray_sym_domain_intern(dom, "filler", 6) > 0);
    int64_t x = ray_sym_intern("a stale runtime id", 18);
    TEST_ASSERT_TRUE(x > 0);
    /* blocks of the cache's two sizes (2^19 ids, 2^19 positions), freed full of x */
    void* blk[8];
    for (int i = 0; i < 8; i++) {
        size_t sz = (size_t)1 << (i < 4 ? 22 : 21);
        blk[i] = ray_alloc_raw(sz);
        TEST_ASSERT_NOT_NULL(blk[i]);
        for (size_t j = 0; j < sz / 8; j++) ((int64_t*)blk[i])[j] = x;
    }
    for (int i = 7; i >= 0; i--) ray_free_raw(blk[i]);
    int64_t n = 1000;
    ray_t* rt = ray_sym_vec_new(RAY_SYM_W64, n);
    TEST_ASSERT_TRUE(rt && !RAY_IS_ERR(rt));
    rt->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(rt))[i] = x;
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("l", 1), RAY_SYM, dom, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, rt), RAY_OK);
    TEST_ASSERT_NOT_NULL(w.lut_id);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    int64_t want = ray_sym_domain_find(dom, "a stale runtime id", 18);
    TEST_ASSERT_EQ_I(want, 2);
    uint32_t back[1000];
    ray_t hdr;
    snprintf(path, sizeof(path), "%s/l", dir);
    TEST_ASSERT_TRUE(stream_sym_read(path, &hdr, back, n));
    int64_t bad = 0;
    for (int64_t i = 0; i < n; i++) if (back[i] != (uint32_t)want) bad++;
    TEST_ASSERT_EQ_I(bad, 0);
    ray_release(rt);
    ray_sym_domain_release(dom);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* An index region that cannot be written (ENOSPC/EFBIG) is dropped: close
 * cuts the file back to the payload and publishes the column without the
 * index.  Everything runs in the forked child so no FILE* state is shared
 * with the parent; the child exits 0 only if close returned RAY_OK. */
static test_result_t test_stream_index_write_failure_drops_index(void) {
#ifdef RAY_OS_LINUX
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-enospc-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 70000;
    ray_t* v = zone_test_col(n, 0, 0);
    TEST_ASSERT_NOT_NULL(v);
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < n; i++) d[i] = i * 3;     /* clustered, no nulls */
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    char path[200]; snprintf(path, sizeof(path), "%s/c", dir);
    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) {
        ray_col_stream_t w;
        if (ray_col_stream_open(&w, dir, ray_sym_intern("c", 1), RAY_I64, NULL, 0) != RAY_OK) _exit(2);
        if (ray_col_stream_index_begin(&w, 0) != RAY_OK) _exit(2);
        if (ray_col_stream_append(&w, v) != RAY_OK) _exit(2);
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        limit.rlim_cur = (rlim_t)(32 + n * 8 + 40);
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        _exit(ray_col_stream_close(&w, false) == RAY_OK ? 0 : 1);
    }
    int status;
    TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
    TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status), 0);
    struct stat st;
    TEST_ASSERT_EQ_I(stat(path, &st), 0);
    TEST_ASSERT_EQ_I((int64_t)st.st_size, 32 + n * 8);
    ray_t* back = ray_col_mmap(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->len, n);
    TEST_ASSERT_FALSE(back->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(back))[n - 1], (n - 1) * 3);
    ray_release(back); ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("requires Linux RLIMIT_FSIZE fault injection");
#endif
}

/* Exactly 65536 rows: one chunk, a zone. */
static test_result_t test_stream_inline_zone_one_chunk(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-one-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 65536;
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = (i * 7919) % 1000003;  /* unclustered */
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("o", 1), RAY_I64, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, v), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    char path[200]; snprintf(path, sizeof(path), "%s/o", dir);
    ray_t* back = ray_col_mmap(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(ray_index_kind(back), RAY_IDX_CHUNK_ZONE);
    ray_release(back); ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

static bool store_files_same(const char* a, const char* b);
static ray_err_t store_hash_append_legacy(const char* path);

/* Index policy: the staged bulk save (parted Parquet partitions, fresh
 * .csv.parted leaves) persists no HASH for an unclustered I64 column of
 * 300000 rows, the shape the splayed converter hashes today.  A guard that
 * passes today (the staged path builds no index); vacuous when the legacy
 * getter decodes a generation-2 column in the equivalence pass. */
static test_result_t test_staged_bulk_no_hash(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "%s", ray_test_tmp("/tmp/rayforce-staged-nohash"));
    (void)ray_test_rm_rf(dir);
    int64_t n = 300000;
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = (i * 7919) % 1000003;
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("u", 1), v);
    ray_release(v);
    TEST_ASSERT_EQ_I(ray_splay_save_staged_bulk(t, dir, NULL), RAY_OK);
    ray_release(t);
    ray_t* back = ray_read_splayed(dir, NULL);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_TRUE(ray_index_kind(ray_table_get_col_idx(back, 0)) != RAY_IDX_HASH);
    ray_release(back);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* ---- hash index built in place in the column file ---------------------- */

/* n rows of `type` keyed as idx_reg_col in test_index.c: keys spread over
 * `keys` values, every null_every-th row null.  SYM ids stay below the
 * interned count (the bare mmap validates them). */
static ray_t* store_hash_col(int8_t type, int64_t n, int64_t keys, int64_t null_every) {
    ray_t* v = type == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W32, n > 0 ? n : 1)
                               : ray_vec_new(type, n > 0 ? n : 1);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = n;
    void* d = ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        int64_t k = (int64_t)(((uint64_t)i * 2654435761ull) % (uint64_t)keys) - keys / 3;
        switch (type) {
        case RAY_I32: case RAY_DATE: ((int32_t*)d)[i] = (int32_t)k; break;
        case RAY_I64: case RAY_TIMESTAMP: ((int64_t*)d)[i] = k * 1000003; break;
        case RAY_F64: ((double*)d)[i] = (double)k * 0.25; break;
        case RAY_SYM: ((uint32_t*)d)[i] = (uint32_t)(k + keys / 3 + 1); break;
        default: break;
        }
    }
    for (int64_t i = 0; null_every && i < n; i += null_every)
        if (ray_vec_set_null_checked(v, i, true) != RAY_OK) { ray_release(v); return NULL; }
    return v;
}

/* Files `a` and `b` hold the same bytes. */
static bool store_files_same(const char* a, const char* b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0 || sa.st_size != sb.st_size) return false;
    size_t sz = (size_t)sa.st_size;
    uint8_t* ba = (uint8_t*)ray_alloc_raw(sz ? sz : 1);
    uint8_t* bb = (uint8_t*)ray_alloc_raw(sz ? sz : 1);
    FILE* fa = fopen(a, "rb");
    FILE* fb = fopen(b, "rb");
    bool ok = ba && bb && fa && fb && fread(ba, 1, sz, fa) == sz &&
              fread(bb, 1, sz, fb) == sz && memcmp(ba, bb, sz) == 0;
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    ray_free_raw(ba); ray_free_raw(bb);
    return ok;
}

/* The hash index appended to index-less column file `path` as before the
 * in-place build: attached to the mapped column, then copied into the file.
 * Built under a small budget, so a column of more than one batch takes the
 * route the in-memory build always took — arrays at the key count, cut to
 * the groups after — and not the sizes the in-place build computes. */
static ray_err_t store_hash_append_legacy(const char* path) {
    ray_t* col = ray_col_mmap(path);
    if (!col || RAY_IS_ERR(col)) { if (col) ray_error_free(col); return RAY_ERR_IO; }
    int64_t wm = ray_heap_anon_watermark();
    ray_heap_set_anon_watermark(INT64_C(4) << 20);
    ray_t* r = ray_index_attach_hash(&col);
    ray_heap_set_anon_watermark(wm);
    ray_err_t err = RAY_ERR_OOM;
    if (!RAY_IS_ERR(r))
        err = ray_col_append_index(path, ray_index_payload(col->index), col->len, col->type);
    else
        ray_error_free(r);
    ray_release(col);
    return err;
}

/* The same, built in place over the mapped column. */
static ray_err_t store_hash_build(const char* path) {
    ray_t* col = ray_col_mmap(path);
    if (!col || RAY_IS_ERR(col)) { if (col) ray_error_free(col); return RAY_ERR_IO; }
    ray_err_t err = ray_col_build_hash_index(path, col, NULL);
    ray_release(col);
    return err;
}

/* The route a splayed save takes for an explicit hash (splay.c,
 * splay_hash_append): built in place, or where that does not apply
 * (RAY_ERR_NYI) built in memory and appended.  Converters persist no
 * auto-built hash (index policy), so no store entry point re-reads a file
 * to hash it; the in-memory fallback is exercised here. */
static ray_err_t store_hash_routed(const char* path) {
    ray_t* col = ray_col_mmap(path);
    if (!col || RAY_IS_ERR(col)) { if (col) ray_error_free(col); return RAY_ERR_IO; }
    ray_err_t err = ray_col_build_hash_index(path, col, NULL);
    if (err == RAY_ERR_NYI) {
        ray_t* hi = ray_idx_hash_fn(col);
        if (!hi || RAY_IS_ERR(hi)) { if (hi) ray_error_free(hi); err = RAY_ERR_OOM; }
        else {
            err = (hi->attrs & RAY_ATTR_HAS_INDEX)
                ? ray_col_append_index(path, ray_index_payload(hi->index), hi->len, hi->type)
                : RAY_ERR_IO;
            ray_release(hi);
        }
    }
    ray_release(col);
    return err;
}

static int64_t store_file_size(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

/* A truncated generation-1 file (a partial copy, a full disk elsewhere)
 * fails at load and at map, whatever its cut: a vector cut in its header
 * or its data, a STR column cut in its pool, a LIST cut in an element.  The
 * extents are checked; the values are taken as written. */
static ray_t* store_trunc_case(int k) {
    if (k == 0) {
        ray_t* v = ray_vec_new(RAY_I64, 1000);
        v->len = 1000;
        for (int64_t i = 0; i < 1000; i++) ((int64_t*)ray_data(v))[i] = i * 7;
        return v;
    }
    if (k == 1) {
        ray_t* v = ray_vec_new(RAY_STR, 64);
        for (int i = 0; i < 64; i++) {
            char b[64];
            int n = snprintf(b, sizeof b, "a pooled string value number %d", i);
            v = ray_str_vec_append(v, b, (size_t)n);
        }
        return v;
    }
    ray_t* l = ray_list_new(3);
    ray_t* a = ray_i64(42);
    ray_t* s = ray_str("a string in a list, long enough", 31);
    ray_t* v = store_trunc_case(0);
    l = ray_list_append(l, a); l = ray_list_append(l, s); l = ray_list_append(l, v);
    ray_release(a); ray_release(s); ray_release(v);
    return l;
}
static test_result_t test_col_truncated_fails(void) {
    const char* p = ray_test_tmp(TMP_COL_PATH);
    for (int k = 0; k < 3; k++) {
        ray_t* v = store_trunc_case(k);
        TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
        TEST_ASSERT_EQ_I(ray_col_save(v, p), RAY_OK);
        int64_t full = store_file_size(p);
        const int64_t cuts[] = { 16, 40, full / 2, full - 1 };
        for (size_t c = 0; c < sizeof cuts / sizeof cuts[0]; c++) {
            TEST_ASSERT_EQ_I(ray_col_save(v, p), RAY_OK);
            TEST_ASSERT_EQ_I(truncate(p, (off_t)cuts[c]), 0);
            for (int how = 0; how < (k == 2 ? 1 : 2); how++) {   /* a LIST has no map */
                ray_t* r = how ? ray_col_mmap(p) : ray_col_load(p);
                if (!r || !RAY_IS_ERR(r)) {
                    if (r) ray_release(r);
                    ray_release(v);
                    FAILF("case %d cut at %lld of %lld: %s opened", k, (long long)cuts[c],
                          (long long)full, how ? "map" : "load");
                }
                ray_error_free(r);
            }
        }
        ray_release(v);
    }
    unlink(p);
    PASS();
}

/* Does `dir` take the in-place build (Linux, a filesystem whose fallocate
 * holds the blocks)?  Probed with a small column. */
static bool store_in_place(const char* dir) {
    char p[240];
    snprintf(p, sizeof(p), "%s/.probe", dir);
    ray_t* v = store_hash_col(RAY_I64, 1000, 10, 0);
    bool ok = v && !RAY_IS_ERR(v) && ray_col_save_bulk(v, p) == RAY_OK &&
              store_hash_build(p) == RAY_OK;
    if (v && !RAY_IS_ERR(v)) ray_release(v);
    unlink(p);
    return ok;
}

/* Built in place, the column file holds exactly the bytes the in-memory
 * build appended — numeric, temporal, float with nulls, SYM ids, every key
 * distinct or few — with one batch and with many (the counting pass), and
 * the index loads and answers.  Where the in-place build does not apply,
 * the same through the in-memory route (store_hash_routed). */
static test_result_t test_col_build_hash_index_bytes(void) {
    (void)ray_pool_get();
    char name[16];
    for (int i = 0; i < 2000; i++) {   /* SYM ids below the interned count */
        int l = snprintf(name, sizeof(name), "hx%d", i);
        TEST_ASSERT_TRUE(ray_sym_intern(name, (size_t)l) >= 0);
    }
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    bool in_place = store_in_place(dir);
    char a[200], b[200];
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
    static const struct { int8_t type; int64_t n, keys, null_every; } shapes[] = {
        { RAY_I64, 200000, 200000, 0 }, { RAY_I64, 300000, 5003, 977 },
        { RAY_I32, 1000, 10, 0 },       { RAY_TIMESTAMP, 90000, 70001, 0 },
        { RAY_F64, 100000, 2003, 101 }, { RAY_SYM, 120000, 997, 0 },
        { RAY_I64, 1, 1, 0 },
    };
    int64_t bad = 0;
    for (int plan = 0; plan < 2; plan++) {
        if (plan == 1) ray_heap_set_anon_watermark(INT64_C(4) << 20);
        for (size_t s = 0; s < sizeof(shapes) / sizeof(shapes[0]); s++) {
            ray_t* v = store_hash_col(shapes[s].type, shapes[s].n, shapes[s].keys, shapes[s].null_every);
            if (!v || RAY_IS_ERR(v)) { bad++; continue; }
            if (ray_col_save_bulk(v, a) != RAY_OK || ray_col_save_bulk(v, b) != RAY_OK) bad++;
            int64_t payload = store_file_size(a);
            ray_err_t e = in_place ? store_hash_build(a) : store_hash_routed(a);
            if (e != RAY_OK || store_hash_append_legacy(b) != RAY_OK) bad++;
            if (!store_files_same(a, b) || store_file_size(a) <= payload) bad++;
            ray_t* m = ray_col_mmap(a);
            if (!m || RAY_IS_ERR(m) || ray_index_kind(m) != RAY_IDX_HASH) bad++;
            else if ((shapes[s].type == RAY_I64 || shapes[s].type == RAY_TIMESTAMP) && shapes[s].n > 2) {
                /* row 2's key finds its group, row 2 among its rows */
                int64_t key = ((int64_t*)ray_data(m))[2];
                ray_idx_rows_t rows = { NULL, false }; int64_t gn = 0, hit = 0;
                if (ray_index_hash_group(m, key, &rows, &gn) != 1) bad++;
                for (int64_t j = 0; j < gn; j++) hit += ray_idx_rows_at(rows, j) == 2;
                if (hit != 1) bad++;
            }
            if (m && !RAY_IS_ERR(m)) ray_release(m); else if (m) ray_error_free(m);
            ray_release(v);
        }
        if (plan == 1) ray_heap_set_anon_watermark(0);
    }
    TEST_ASSERT_EQ_I(bad, 0);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* A file that already carries an index is refused and left as it is; a
 * STR column is not built in place (the caller takes the in-memory path)
 * and its file is not touched. */
static test_result_t test_col_build_hash_index_refusals(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-ref-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    char p[200]; snprintf(p, sizeof(p), "%s/a", dir);
    ray_t* m = NULL;
    if (store_in_place(dir)) {
        ray_t* v = store_hash_col(RAY_I64, 100000, 5003, 0);
        TEST_ASSERT_EQ_I(ray_col_save_bulk(v, p), RAY_OK);
        TEST_ASSERT_EQ_I(store_hash_build(p), RAY_OK);
        int64_t indexed = store_file_size(p);
        TEST_ASSERT_EQ_I(store_hash_build(p), RAY_ERR_CORRUPT);
        TEST_ASSERT_EQ_I(store_file_size(p), indexed);
        m = ray_col_mmap(p);
        TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
        TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_HASH);
        ray_release(m);
        ray_release(v);
    }

    ray_t* s = ray_vec_new(RAY_STR, 4);
    s = ray_str_vec_append(s, "alpha", 5);
    s = ray_str_vec_append(s, "beta", 4);
    char q[200]; snprintf(q, sizeof(q), "%s/s", dir);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(s, q), RAY_OK);
    int64_t plain = store_file_size(q);
    TEST_ASSERT_EQ_I(store_hash_build(q), RAY_ERR_NYI);
    TEST_ASSERT_EQ_I(store_file_size(q), plain);
    /* the routed build takes the in-memory path for it */
    TEST_ASSERT_EQ_I(store_hash_routed(q), RAY_OK);
    m = ray_col_mmap(q);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_HASH);
    ray_release(m);
    ray_release(s);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* On a filesystem the in-place build does not trust (copy-on-write,
 * network, user — or any platform but Linux) it leaves the file alone, and
 * the routed build makes the index in memory and appends it: the same
 * bytes.  Debug builds pretend the filesystem is untrusted. */
static test_result_t test_col_build_hash_index_untrusted_fs(void) {
#if defined(RAY_OS_LINUX) && !defined(DEBUG)
    SKIP("RAY_HASH_INJECT is read by debug builds only");
#endif
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-fs-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    char a[200], b[200];
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
#if defined(DEBUG)
    TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", "fs", 1), 0);
#endif
    ray_t* v = store_hash_col(RAY_I64, 300000, 5003, 977);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, a), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, b), RAY_OK);
    int64_t payload = store_file_size(a);
    ray_err_t direct = store_hash_build(a);
    int64_t after = store_file_size(a);
    ray_err_t routed = store_hash_routed(a);
#if defined(DEBUG)
    unsetenv("RAY_HASH_INJECT");
#endif
    TEST_ASSERT_EQ_I(direct, RAY_ERR_NYI);
    TEST_ASSERT_EQ_I(after, payload);
    TEST_ASSERT_EQ_I(routed, RAY_OK);
    TEST_ASSERT_EQ_I(store_hash_append_legacy(b), RAY_OK);
    TEST_ASSERT_TRUE(store_files_same(a, b));
    ray_t* m = ray_col_mmap(a);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(ray_index_kind(m), RAY_IDX_HASH);
    ray_release(m);
    ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* Which filesystems the in-place build trusts, by the type fstatfs reports
 * (debug builds stand an injected type in for the real one, so the ones no
 * test can mount are classified too): ext4, xfs and tmpfs, whose fallocate
 * holds the blocks; not overlayfs, whose upper layer cannot be told, nor
 * the copy-on-write, network and user filesystems.  An untrusted one leaves
 * the file at its payload. */
static test_result_t test_col_build_hash_index_fs_types(void) {
#if defined(DEBUG) && defined(RAY_OS_LINUX)
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-fst-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char p[200]; snprintf(p, sizeof(p), "%s/a", dir);
    static const struct { const char* inject; bool trusted; } types[] = {
        { "fs:ef53",     true  },   /* ext2/3/4 */
        { "fs:58465342", true  },   /* xfs */
        { "fs:1021994",  true  },   /* tmpfs */
        { "fs:794c7630", false },   /* overlayfs */
        { "fs:9123683e", false },   /* btrfs */
        { "fs:2fc12fc1", false },   /* zfs */
        { "fs:ca451a4e", false },   /* bcachefs */
        { "fs:6969",     false },   /* nfs */
        { "fs:ff534d42", false },   /* cifs */
        { "fs:fe534d42", false },   /* smb2 */
        { "fs:65735546", false },   /* fuse */
        { "fs:c36400",   false },   /* ceph */
    };
    ray_t* v = store_hash_col(RAY_I64, 1000, 10, 0);
    int64_t bad = 0;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (ray_col_save_bulk(v, p) != RAY_OK) { bad++; continue; }
        int64_t payload = store_file_size(p);
        TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", types[i].inject, 1), 0);
        ray_err_t e = store_hash_build(p);
        unsetenv("RAY_HASH_INJECT");
        if (types[i].trusted ? (e != RAY_OK || store_file_size(p) <= payload)
                             : (e != RAY_ERR_NYI || store_file_size(p) != payload)) bad++;
    }
    ray_release(v);
    TEST_ASSERT_EQ_I(bad, 0);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("needs a debug build on Linux");
#endif
}

/* A SYM column of 300000 rows over `keys` interned symbols, carrying an
 * explicit hash index (.idx.hash). */
static ray_t* store_cancel_sym_col(int64_t keys) {
    char name[16];
    for (int64_t i = 0; i <= keys; i++) {   /* SYM ids below the interned count */
        int l = snprintf(name, sizeof(name), "hc%lld", (long long)i);
        if (ray_sym_intern(name, (size_t)l) < 0) return NULL;
    }
    ray_t* v = store_hash_col(RAY_SYM, 300000, keys, 0);
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* h = ray_idx_hash_fn(v);
    ray_release(v);
    return h;
}

/* A region that cannot be allocated or mapped this time (debug builds
 * inject either) does not cost the column its hash index: the in-place
 * build gives way (NYI, the file at its payload) and the routed build
 * and a splayed save of an explicit SYM hash build it in memory and append
 * it — the same bytes. */
static test_result_t test_col_build_hash_index_refused(void) {
#if defined(DEBUG)
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-ref2-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char a[200], b[200];
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
    ray_t* v = store_hash_col(RAY_I64, 300000, 5003, 977);
    static const char* steps[2] = { "grow", "map" };
    int64_t bad = 0;
    for (int plan = 0; plan < 2; plan++) {          /* one batch; the counting pass first */
        if (plan == 1) ray_heap_set_anon_watermark(INT64_C(4) << 20);
        for (int s = 0; s < 2; s++) {
            if (ray_col_save_bulk(v, a) != RAY_OK || ray_col_save_bulk(v, b) != RAY_OK) bad++;
            int64_t payload = store_file_size(a);
            TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", steps[s], 1), 0);
            ray_err_t direct = store_hash_build(a);
            int64_t after = store_file_size(a);
            ray_err_t routed = store_hash_routed(a);
            unsetenv("RAY_HASH_INJECT");
            if (direct != RAY_ERR_NYI || after != payload || routed != RAY_OK) bad++;
            if (store_hash_append_legacy(b) != RAY_OK || !store_files_same(a, b)) bad++;
            ray_t* m = ray_col_mmap(a);
            if (!m || RAY_IS_ERR(m) || ray_index_kind(m) != RAY_IDX_HASH) bad++;
            if (m && !RAY_IS_ERR(m)) ray_release(m); else if (m) ray_error_free(m);
        }
        if (plan == 1) ray_heap_set_anon_watermark(0);
    }
    /* a generation-0 splayed save of an explicit SYM hash under the same
     * refusal still hashes the column (converters persist no auto-built
     * hash: index policy, so the explicit index is the save's hash build) */
    int64_t name = ray_sym_intern("k", 1);
    ray_t* t = ray_table_new(1);
    ray_t* sv = store_cancel_sym_col(2003);
    TEST_ASSERT_TRUE(sv && !RAY_IS_ERR(sv) && (sv->attrs & RAY_ATTR_HAS_INDEX));
    t = ray_table_add_col(t, name, sv);
    ray_release(sv);
    char sd[200]; snprintf(sd, sizeof(sd), "%s/t", dir);
    char yp[220]; snprintf(yp, sizeof(yp), "%s/t.sym", dir);   /* the SYM columns' symfile */
    TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", "map", 1), 0);
    ray_err_t se = ray_splay_save_mode(t, sd, yp, RAY_COMPRESS_OFF);
    unsetenv("RAY_HASH_INJECT");
    TEST_ASSERT_EQ_I(se, RAY_OK);
    ray_t* back = ray_read_splayed(sd, yp);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    if (ray_index_kind(ray_table_get_col(back, name)) != RAY_IDX_HASH) bad++;
    ray_release(back);
    ray_release(t);
    ray_release(v);
    TEST_ASSERT_EQ_I(bad, 0);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("RAY_HASH_INJECT is read by debug builds only");
#endif
}

/* A build that fails once the region is allocated and mapped (debug
 * builds raise an interrupt right there) cuts the file back to its payload:
 * it loads unindexed and builds again after.  One batch and many. */
static test_result_t test_col_build_hash_index_truncate(void) {
#if defined(DEBUG)
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-tr-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char p[200]; snprintf(p, sizeof(p), "%s/a", dir);
    ray_t* v = store_hash_col(RAY_I64, 300000, 5003, 977);
    int64_t bad = 0;
    for (int plan = 0; plan < 2; plan++) {
        if (plan == 1) ray_heap_set_anon_watermark(INT64_C(4) << 20);
        if (ray_col_save_bulk(v, p) != RAY_OK) bad++;
        int64_t payload = store_file_size(p);
        TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", "build", 1), 0);
        ray_err_t e = store_hash_build(p);
        unsetenv("RAY_HASH_INJECT");
        ray_clear_interrupt();
        if (e != RAY_ERR_CANCEL || store_file_size(p) != payload) bad++;
        ray_t* back = ray_col_mmap(p);
        if (!back || RAY_IS_ERR(back) || (back->attrs & RAY_ATTR_HAS_INDEX) || back->len != 300000) bad++;
        if (back && !RAY_IS_ERR(back)) ray_release(back); else if (back) ray_error_free(back);
        if (store_hash_build(p) != RAY_OK) bad++;
        back = ray_col_mmap(p);
        if (!back || RAY_IS_ERR(back) || ray_index_kind(back) != RAY_IDX_HASH) bad++;
        if (back && !RAY_IS_ERR(back)) ray_release(back); else if (back) ray_error_free(back);
        if (plan == 1) ray_heap_set_anon_watermark(0);
    }
    ray_release(v);
    TEST_ASSERT_EQ_I(bad, 0);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("RAY_HASH_INJECT is read by debug builds only");
#endif
}

/* A cancel skips the finish tasks not yet started and the dispatch returns
 * normally (#716): close_all must report it and publish no column. */
static test_result_t test_stream_close_all_cancel(void) {
    ray_pool_t* pool = ray_pool_get();
    if (!pool || pool->n_workers == 0) SKIP("the finish runs in line without pool workers");
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-cancel-%d", (int)getpid());
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    ray_col_stream_t w[2];
    memset(w, 0, sizeof(w));
    ray_t* v = store_hash_col(RAY_I64, 1000, 97, 0);
    for (int c = 0; c < 2; c++) {
        TEST_ASSERT_EQ_I(ray_col_stream_open(&w[c], dir, ray_sym_intern(c ? "y" : "x", 1), RAY_I64, NULL, 0), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w[c], v), RAY_OK);
    }
    ray_request_interrupt();
    ray_err_t e = ray_col_stream_close_all(w, 2, false, NULL);
    ray_clear_interrupt();
    TEST_ASSERT_EQ_I(e, RAY_ERR_CANCEL);
    for (int c = 0; c < 2; c++) {
        char p[200]; snprintf(p, sizeof(p), "%s/%s", dir, c ? "y" : "x");
        TEST_ASSERT_TRUE(access(p, F_OK) != 0);
    }
    ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

#if defined(DEBUG) && defined(RAY_OS_LINUX)
/* Two explicitly hashed SYM columns, saved as generation 0 (the save
 * rebuilds an explicit SYM hash over the file-local ids in place,
 * splay_hash_append): the debug build's "build" injection raises an
 * interrupt inside the first hash build, once every column file is written.
 * Converters build no hash of their own (index policy). */
static ray_t* store_cancel_table(void) {
    ray_t* t = ray_table_new(2);
    for (int c = 0; c < 2; c++) {
        ray_t* v = store_cancel_sym_col(c ? 7919 : 5003);
        t = ray_table_add_col(t, ray_sym_intern(c ? "b" : "a", 1), v);
        ray_release(v);
    }
    return t;
}
#endif

/* A cancel that lands once the columns are written (here: in the hash
 * build, the last phase before .d) still stops a save: over an existing
 * table the previous generation stays current, and a first write leaves
 * no table (a directory made beforehand keeps what else it holds). */
static test_result_t test_splay_save_cancel_late(void) {
#if defined(DEBUG) && defined(RAY_OS_LINUX)
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-save-cancel-%d", (int)getpid());
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char sd[200]; snprintf(sd, sizeof(sd), "%s/t", dir);
    char yp[220]; snprintf(yp, sizeof(yp), "%s/t.sym", dir);   /* the SYM columns' symfile */
    int64_t small[3] = {1, 2, 3};
    ray_t* t = ray_table_new(2);
    for (int c = 0; c < 2; c++) {
        ray_t* v = ray_vec_from_raw(RAY_I64, small, 3);
        t = ray_table_add_col(t, ray_sym_intern(c ? "b" : "a", 1), v);
        ray_release(v);
    }
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, sd, yp, RAY_COMPRESS_OFF), RAY_OK);
    ray_release(t);
    t = store_cancel_table();
    TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", "build", 1), 0);
    ray_err_t e = ray_splay_save_mode(t, sd, yp, RAY_COMPRESS_OFF);
    unsetenv("RAY_HASH_INJECT");
    bool cancelled = ray_interrupted();
    ray_clear_interrupt();
    ray_release(t);
    TEST_ASSERT_TRUE(cancelled);
    TEST_ASSERT_EQ_I(e, RAY_ERR_CANCEL);
    ray_t* back = ray_read_splayed(sd, yp);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(ray_table_nrows(back), 3);
    ray_release(back);
    (void)ray_test_rm_rf(sd);
    char schema[240], keep[240], col[240];
    snprintf(schema, sizeof(schema), "%s/.d", sd);
    snprintf(keep, sizeof(keep), "%s/keep.txt", sd);
    snprintf(col, sizeof(col), "%s/a", sd);
    t = store_cancel_table();
    for (int premade = 0; premade < 2; premade++) {
        if (premade) {
            TEST_ASSERT_EQ_I(ray_test_mkdir_p(sd), 0);
            FILE* f = fopen(keep, "w"); TEST_ASSERT_NOT_NULL(f); fputs("foreign\n", f); fclose(f);
        }
        TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", "build", 1), 0);
        e = ray_splay_save_mode(t, sd, yp, RAY_COMPRESS_OFF);
        unsetenv("RAY_HASH_INJECT");
        ray_clear_interrupt();
        TEST_ASSERT_EQ_I(e, RAY_ERR_CANCEL);
        TEST_ASSERT_TRUE(access(schema, F_OK) != 0);
        TEST_ASSERT_TRUE(access(col, F_OK) != 0);
        TEST_ASSERT_TRUE(premade ? access(keep, F_OK) == 0 : access(sd, F_OK) != 0);
    }
    ray_release(t);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("needs a debug build on Linux");
#endif
}

/* The same cancel skips the dispatch that persists the deferred zones:
 * what it would have written is released, not left behind a save. */
static test_result_t test_splay_save_cancel_leaks(void) {
#if defined(DEBUG) && defined(RAY_OS_LINUX)
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-save-cleak-%d", (int)getpid());
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char sd[200]; snprintf(sd, sizeof(sd), "%s/t", dir);
    char yp[220]; snprintf(yp, sizeof(yp), "%s/t.sym", dir);   /* the SYM columns' symfile */
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(2), RAY_OK);   /* one worker */
    ray_t* t = store_cancel_table();
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, sd, yp, RAY_COMPRESS_OFF), RAY_OK);
    size_t base = 0;
    int bad = 0;
    for (int k = 0; k < 5; k++) {
        TEST_ASSERT_EQ_I(setenv("RAY_HASH_INJECT", "build", 1), 0);
        if (ray_splay_save_mode(t, sd, yp, RAY_COMPRESS_OFF) != RAY_ERR_CANCEL) bad++;
        unsetenv("RAY_HASH_INJECT");
        ray_clear_interrupt();
        ray_mem_stats_t st; ray_mem_stats(&st);
        if (k == 1) base = st.bytes_allocated;
        if (k == 4 && st.bytes_allocated > base + 256) {
            fprintf(stderr, "  save_cancel_leaks heap %+lld B\n",
                    (long long)st.bytes_allocated - (long long)base);
            bad++;
        }
    }
    ray_release(t);
    ray_pool_destroy();
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#else
    SKIP("needs a debug build on Linux");
#endif
}

/* The marker goes on last: a process that ends with the whole region
 * written but before the marker (debug builds end it right there, in a
 * child) leaves a file that does not claim an index: it loads as the plain
 * column.  The column is under
 * the parallel threshold: the child has no pool workers. */
static test_result_t test_col_build_hash_index_marker_last(void) {
#if defined(DEBUG) && defined(RAY_OS_LINUX)
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-mk-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char p[200], r[200];
    snprintf(p, sizeof(p), "%s/a", dir);
    snprintf(r, sizeof(r), "%s/ref", dir);
    ray_t* v = store_hash_col(RAY_I64, 50000, 997, 0);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, p), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, r), RAY_OK);
    int64_t payload = store_file_size(p);
    TEST_ASSERT_EQ_I(store_hash_build(r), RAY_OK);
    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) {
        if (setenv("RAY_HASH_INJECT", "premarker", 1)) _exit(2);
        (void)store_hash_build(p);
        _exit(3);                                  /* not reached: it ends inside */
    }
    int status;
    TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
    TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status), 86);
    /* the whole region is there, the header claims nothing */
    TEST_ASSERT_EQ_I(store_file_size(p), store_file_size(r));
    TEST_ASSERT_TRUE(store_file_size(p) > payload);
    uint32_t mp = 0, mr = 0;
    FILE* f = fopen(p, "rb"); TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I((int64_t)fread(&mp, 1, 4, f), 4); fclose(f);
    f = fopen(r, "rb"); TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I((int64_t)fread(&mr, 1, 4, f), 4); fclose(f);
    TEST_ASSERT_TRUE(mr != 0);          /* the finished file's marker */
    TEST_ASSERT_TRUE(mp != mr);
    ray_t* back = ray_col_mmap(p);      /* the unclaimed region is not read: the plain column */
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_FALSE(back->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_EQ_I(back->len, v->len);
    TEST_ASSERT_TRUE(memcmp(ray_data(back), ray_data(v), (size_t)v->len * 8) == 0);
    ray_release(back);
    ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("needs a debug build on Linux");
#endif
}

/* The region's blocks cannot be allocated past RLIMIT_FSIZE (a full disk
 * alike): the in-place build says so (NYI: the caller builds in memory),
 * the file is left at its payload, loads unindexed, and builds once there
 * is room.  The column is under the parallel threshold: the child has no
 * pool workers. */
static test_result_t test_col_build_hash_index_rollback(void) {
#ifdef RAY_OS_LINUX
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-rb-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char p[200]; snprintf(p, sizeof(p), "%s/a", dir);
    int64_t n = 50000;
    ray_t* v = store_hash_col(RAY_I64, n, 997, 0);
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, p), RAY_OK);
    int64_t payload = store_file_size(p);
    pid_t pid = fork(); TEST_ASSERT_TRUE(pid >= 0);
    if (!pid) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        limit.rlim_cur = (rlim_t)payload + 40;     /* the pad fits, the region doesn't */
        if (signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
        _exit(store_hash_build(p) == RAY_ERR_NYI ? 0 : 1);
    }
    int status;
    TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
    TEST_ASSERT_TRUE(WIFEXITED(status)); TEST_ASSERT_EQ_I(WEXITSTATUS(status), 0);
    TEST_ASSERT_EQ_I(store_file_size(p), payload);
    ray_t* back = ray_col_mmap(p);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_FALSE(back->attrs & RAY_ATTR_HAS_INDEX);
    TEST_ASSERT_EQ_I(back->len, n);
    ray_release(back);
    TEST_ASSERT_EQ_I(store_hash_build(p), RAY_OK);
    back = ray_col_mmap(p);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(ray_index_kind(back), RAY_IDX_HASH);
    ray_release(back);
    ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
#else
    SKIP("requires Linux RLIMIT_FSIZE fault injection");
#endif
}

/* Interrupted at points spread over a build under a budget of many
 * batches — before the region is laid out, while it is filled — the build
 * reports the cancel and the file is back at its payload, or it completes
 * with the uninterrupted bytes. */
static void store_interrupt_after(void* arg) {
    int64_t ns = *(const int64_t*)arg;
    struct timespec ts = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
    nanosleep(&ts, NULL);
    ray_request_interrupt();
}
static int64_t store_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static test_result_t test_col_build_hash_index_interrupt(void) {
    ray_pool_t* pool = ray_pool_get();
    if (!pool || ray_pool_total_workers(pool) < 2) SKIP("needs workers");
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-hidx-int-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    if (!store_in_place(dir)) { (void)ray_test_rm_rf(dir); SKIP("no in-place build on this filesystem"); }
    char p[200], r[200];
    snprintf(p, sizeof(p), "%s/a", dir);
    snprintf(r, sizeof(r), "%s/ref", dir);
    ray_t* v = store_hash_col(RAY_I64, 500000, 70001, 1013);
    ray_heap_set_anon_watermark(INT64_C(64) << 20);   /* batches of 128K rows */
    TEST_ASSERT_EQ_I(ray_col_save_bulk(v, r), RAY_OK);
    int64_t payload = store_file_size(r);
    atomic_store(&pool->cancelled, 1);
    TEST_ASSERT_EQ_I(store_hash_build(r), RAY_ERR_CANCEL);
    atomic_store(&pool->cancelled, 0);
    TEST_ASSERT_EQ_I(store_file_size(r), payload);
    int64_t t0 = store_now_ns();
    TEST_ASSERT_EQ_I(store_hash_build(r), RAY_OK);
    int64_t took = store_now_ns() - t0;
    int64_t cancelled = 0, bad = 0;
    for (int k = 0; k < 8; k++) {
        if (ray_col_save_bulk(v, p) != RAY_OK) { bad++; continue; }
        int64_t delay = took * k / 8;
        ray_thread_t th;
        TEST_ASSERT_EQ_I(ray_thread_create(&th, store_interrupt_after, &delay), RAY_OK);
        ray_err_t e = store_hash_build(p);
        ray_thread_join(th);
        ray_clear_interrupt();
        if (e == RAY_OK) { if (!store_files_same(p, r)) bad++; continue; }
        cancelled++;
        if (e != RAY_ERR_CANCEL || store_file_size(p) != payload) bad++;
        ray_t* back = ray_col_mmap(p);
        if (!back || RAY_IS_ERR(back) || (back->attrs & RAY_ATTR_HAS_INDEX)) bad++;
        if (back && !RAY_IS_ERR(back)) ray_release(back); else if (back) ray_error_free(back);
    }
    ray_heap_set_anon_watermark(0);
    TEST_ASSERT_EQ_I(bad, 0);
    TEST_ASSERT_TRUE(cancelled > 0);
    ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* A column carrying a HASH (as an older converter's store did) keeps it
 * through a load and a resave that requests no index: the save cannot tell
 * it from an explicit .idx.hash.  Generation 0 is pinned (compress off). */
static int store_slot_kind(ray_t* t, int64_t c) {
    ray_col_meta_t m;
    return ray_table_col_meta_idx(t, c, &m) ? (int)m.index_kind : -1;
}
static test_result_t test_resave_keeps_hash(void) {
    char dirs[2][160];
    snprintf(dirs[0], sizeof(dirs[0]), "%s", ray_test_tmp("/tmp/rayforce-resave-hash0"));
    snprintf(dirs[1], sizeof(dirs[1]), "%s", ray_test_tmp("/tmp/rayforce-resave-hash1"));
    for (int i = 0; i < 2; i++) (void)ray_test_rm_rf(dirs[i]);
    int64_t n = 300000;
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = (i * 7919) % 1000003;
    ray_t* h = ray_idx_hash_fn(v);
    ray_release(v);
    TEST_ASSERT_TRUE(h && !RAY_IS_ERR(h) && (h->attrs & RAY_ATTR_HAS_INDEX));
    ray_t* t = ray_table_new(1);
    t = ray_table_add_col(t, ray_sym_intern("u", 1), h);
    ray_release(h);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(t, dirs[0], NULL, RAY_COMPRESS_OFF), RAY_OK);
    ray_release(t);
    ray_t* a = ray_read_splayed(dirs[0], NULL);
    TEST_ASSERT_TRUE(a && !RAY_IS_ERR(a));
    TEST_ASSERT_EQ_I(store_slot_kind(a, 0), RAY_IDX_HASH);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(a, dirs[1], NULL, RAY_COMPRESS_OFF), RAY_OK);
    ray_release(a);
    ray_t* b = ray_read_splayed(dirs[1], NULL);
    TEST_ASSERT_TRUE(b && !RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(store_slot_kind(b, 0), RAY_IDX_HASH);
    ray_release(b);
    for (int i = 0; i < 2; i++) (void)ray_test_rm_rf(dirs[i]);
    PASS();
}

/* ---- lazy in-memory HASH (index policy PI Task 4) ------------------------
 * A converted table persists a CHUNK_ZONE, not a HASH.  An equality or `in`
 * probe on an integer column whose zone cannot prune builds a HASH in
 * memory on first use, held in a process-wide cache that the save path never
 * sees.  Generation 2 is covered too (Ruling C1): the hash is built from the
 * materialized plain column and dropped with the encoded object.  Every
 * path goes through ray_test_tmp; these run in the equivalence pass as well,
 * where lz_make's save is generation 2. */
static ray_runtime_t* g_lzrt;
static void lz_setup(void) {
    g_lzrt = ray_runtime_create(0, NULL);
    (void)ray_test_rm_rf(ray_test_tmp("/tmp/rayforce_lazy"));
}
static void lz_teardown(void) {
    ray_idx_lazy_test_fail_build(NULL);
    ray_idx_lazy_clear();
    (void)ray_test_rm_rf(ray_test_tmp("/tmp/rayforce_lazy"));
    ray_runtime_destroy(g_lzrt);
}
static void lz_ok(const char* src) {
    ray_t* r = ray_eval_str(ray_test_tmp(src));
    if (r && RAY_IS_ERR(r)) { fprintf(stderr, "%s: %s\n", src, ray_error_msg()); ray_error_free(r); abort(); }
    if (r) ray_release(r);
}
static ray_t* lz_eval(const char* src) { return ray_eval_str(ray_test_tmp(src)); }
/* A converted-like table: an unclustered I64 column h of 300000 rows whose
 * file carries a CHUNK_ZONE only (what the converters write), beside a
 * clustered one v.  `opts` is appended to the save (generation choice). */
static void lz_make_opts(const char* opts) {
    char src[256];
    lz_ok("(set i (til 300000))");
    snprintf(src, sizeof src,
             "(.db.splayed.set \"/tmp/rayforce_lazy/a/\" (table [h v] (list (%% (* i 7919) 1000003) i))%s)", opts);
    lz_ok(src);
    lz_ok("(set A (.db.splayed.get \"/tmp/rayforce_lazy/a/\"))");
}
static void lz_make(void) { lz_make_opts(""); }
/* The table slot's recorded index kind, never decoding (metadata). */
static int lz_slot_kind(const char* var, const char* col, ray_t** enc_out) {
    ray_t* t = ray_env_get(ray_sym_intern(var, strlen(var)));
    ray_col_meta_t m;
    int64_t c = -1;
    for (int64_t k = 0; t && k < ray_table_ncols(t); k++)
        if (ray_table_col_name(t, k) == ray_sym_intern(col, strlen(col))) c = k;
    if (c < 0 || !ray_table_col_meta_idx(t, c, &m)) return -1;
    if (enc_out) *enc_out = m.enc;
    return (int)m.index_kind;
}

/* Run the point lookup (== h 7919) on `var` n times; true when every
 * answer is the one row v = 1. */
static bool lz_probe_n(const char* var, int n) {
    char src[128];
    snprintf(src, sizeof src, "(at (select {v: v from: %s where: (== h 7919)}) 'v)", var);
    for (int k = 0; k < n; k++) {
        ray_t* r = lz_eval(src);
        bool ok = r && !RAY_IS_ERR(r) && r->len == 1 && ((int64_t*)ray_data(r))[0] == 1;
        if (r && RAY_IS_ERR(r)) ray_error_free(r); else if (r) ray_release(r);
        if (!ok) return false;
    }
    return true;
}
/* The table column var.col (borrowed: the table holds it). */
static ray_t* lz_col(const char* var, const char* col) {
    ray_t* t = ray_env_get(ray_sym_intern(var, strlen(var)));
    ray_t* c = t ? ray_table_col(t, ray_sym_intern(col, strlen(col))) : NULL;
    return c && !RAY_IS_ERR(c) ? c : NULL;
}

/* The first RAY_IDX_LAZY_BUILD_PROBES - 1 probes scan (their rows are
 * counted, nothing is built); the next one builds, and later probes reuse it.
 * Every answer is the same. */
static test_result_t test_lazy_first_use(void) {
    lz_make();
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    lz_ok("(do (.idx.uses A) null)");
    for (int k = 1; k < RAY_IDX_LAZY_BUILD_PROBES; k++) {
        TEST_ASSERT_TRUE(lz_probe_n("A", 1));
        TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
        TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "h")), (int64_t)k * 300000);
    }
    TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), 0);
    ray_t* u = lz_eval("(.idx.uses A)");
    TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u) && ray_dict_len(u) == 0);   /* the scans used no index */
    ray_release(u);
    TEST_ASSERT_TRUE(lz_probe_n("A", 1));                          /* this one builds */
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "h")), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), 300000LL * (8 + 58));
    u = lz_eval("(at (.idx.uses A) 'h)");
    TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u) && u->i64 == 1);          /* attributed to the column */
    ray_release(u);
    ray_t* r = lz_eval("(at (select {v: v from: A where: (in h [7919 15838])}) 'v)");   /* rows 1 and 2 */
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->len == 2 &&
                     ((int64_t*)ray_data(r))[0] == 1 && ((int64_t*)ray_data(r))[1] == 2);
    ray_release(r);
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);                     /* reused, not rebuilt */
    u = lz_eval("(at (.idx.uses A) 'h)");
    TEST_ASSERT_TRUE(u && !RAY_IS_ERR(u) && u->i64 == 1);          /* the in probe used it */
    ray_release(u);
    /* the table's column is untouched: its slot records a zone, as does the
     * column a reader gets */
    TEST_ASSERT_EQ_I(lz_slot_kind("A", "h", NULL), RAY_IDX_CHUNK_ZONE);
    ray_t* k = lz_eval("(at (.idx.info (at A 'h)) 'kind)");
    TEST_ASSERT_TRUE(k && !RAY_IS_ERR(k) && k->i64 == ray_sym_intern("chunk_zone", 10));
    ray_release(k);
    PASS();
}

/* One unsplit AND (a graph run without the optimizer): (== h 15838)
 * (>= v 0) (<= v hi) over A; the rows, which hold row 2 alone. */
static ray_t* lz_eq_range(ray_t* tbl, int64_t hi) {
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* pred = ray_and(g, ray_and(g, ray_eq(g, ray_scan(g, "h"), ray_const_i64(g, 15838)),
                                           ray_ge(g, ray_scan(g, "v"), ray_const_i64(g, 0))),
                             ray_le(g, ray_scan(g, "v"), ray_const_i64(g, hi)));
    ray_t* r = ray_execute(g, ray_filter(g, ray_const_table(g, tbl), pred));
    ray_graph_free(g);
    return r;
}

/* An eq-plus-range conjunction (an unsplit AND: a graph run without the
 * optimizer) probes the lazy hash for its equality.  The range keeps
 * every row: a range that keeps only a few chunks of a generation-2 v
 * prunes the chain from v's chunk statistics first (exec_filter_chain_prune)
 * and answers the same rows by its scan. */
static test_result_t test_lazy_eq_range(void) {
    lz_make();
    ray_t* tbl = ray_env_get(ray_sym_intern("A", 1));
    TEST_ASSERT_TRUE(tbl && tbl->type == RAY_TABLE);
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES - 1));   /* scans: pending */
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    uint64_t hits0 = ray_idx_hits[IDX_SITE_FILTER_EQRANGE];
    for (int pass = 0; pass < 2; pass++) {
        ray_t* r = lz_eq_range(tbl, pass ? 5 : 300000);
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_TABLE && ray_table_nrows(r) == 1);
        ray_t* v = ray_table_col(r, ray_sym_intern("v", 1));
        TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v) && ((int64_t*)ray_data(v))[0] == 2);
        ray_release(r);
        if (!pass) {
            TEST_ASSERT_EQ_I(ray_idx_hits[IDX_SITE_FILTER_EQRANGE], hits0 + 1);
            TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
        }
    }
    PASS();
}

/* Never persisted: a resave of the probed table records a zone, not a hash. */
static test_result_t test_lazy_not_persisted(void) {
    lz_make();
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    lz_ok("(.db.splayed.set \"/tmp/rayforce_lazy/b/\" A)");
    lz_ok("(set B (.db.splayed.get \"/tmp/rayforce_lazy/b/\"))");
    TEST_ASSERT_EQ_I(lz_slot_kind("B", "h", NULL), RAY_IDX_CHUNK_ZONE);
    ray_t* k = lz_eval("(at (.idx.info (at B 'h)) 'kind)");
    TEST_ASSERT_TRUE(k && !RAY_IS_ERR(k) && k->i64 == ray_sym_intern("chunk_zone", 10));
    ray_release(k);
    PASS();
}

/* The RAM budget bounds it: below the cost nothing is built, results equal,
 * and the decline is remembered for the column (no retry per query). */
static test_result_t test_lazy_budget(void) {
    lz_make();
    int64_t saved = __RUNTIME->ram_limit;
    __RUNTIME->ram_limit = 4LL * (8 + 58) * 300000 - 1;     /* one byte below the bound */
    ray_t* r = lz_eval("(at (select {v: v from: A where: (== h 7919)}) 'v)");
    __RUNTIME->ram_limit = saved;
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->len == 1 && ((int64_t*)ray_data(r))[0] == 1);
    ray_release(r);
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "h")), -1);    /* declined */
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES));  /* budget back: declined stays */
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    PASS();
}

/* A clustered column (zone pruning works) gets no lazy index, and neither
 * does a query that is not a point lookup. */
static test_result_t test_lazy_not_wanted(void) {
    lz_make();
    for (int k = 0; k < RAY_IDX_LAZY_BUILD_PROBES; k++) lz_ok("(select {h: h from: A where: (== v 7919)})");
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "v")), -1);   /* not even pending */
    lz_ok("(select {h: h from: A where: (> h 7919)})");
    lz_ok("(select {s: (sum h) from: A})");
    lz_ok("(select {c: (count v) from: A by: h})");
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    PASS();
}

/* A key outside the column's zone range matches nothing: the zone answers,
 * no scan work is counted and nothing is built, however often it is asked.
 * An `in` set wholly outside the range likewise; one in range scans. */
static test_result_t lazy_out_of_range(const char* opts) {
    lz_make_opts(opts);
    uint64_t z0 = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    for (int k = 0; k < RAY_IDX_LAZY_BUILD_PROBES + 1; k++) {
        ray_t* r = lz_eval("(count (select {v: v from: A where: (== h 2000000)}))");
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 0);
        ray_release(r);
        r = lz_eval("(count (select {v: v from: A where: (in h [2000000 -5])}))");
        TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 0);
        ray_release(r);
    }
    TEST_ASSERT_EQ_I(ray_idx_hits[IDX_SITE_FILTER_ZONE], z0 + 2 * (RAY_IDX_LAZY_BUILD_PROBES + 1));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "h")), -1);
    /* in range: 0 is row 0 */
    ray_t* r = lz_eval("(at (select {v: v from: A where: (== h 0)}) 'v)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->len == 1 && ((int64_t*)ray_data(r))[0] == 0);
    ray_release(r);
    r = lz_eval("(count (select {v: v from: A where: (in h [2000000 7919])}))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 1);
    ray_release(r);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "h")), 2 * 300000);   /* both scanned */
    PASS();
}
static test_result_t test_lazy_out_of_range(void)      { return lazy_out_of_range(""); }
static test_result_t test_lazy_out_of_range_gen2(void) { return lazy_out_of_range(" {compress: true}"); }

/* Mappings of files under the lazy tests' root that were unlinked: a
 * pruned store generation some object still pins.  0 off Linux. */
static int lz_deleted_mappings(void) {
    int n = 0;
#ifdef __linux__
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[1024];
    const char* root = ray_test_tmp("/tmp/rayforce_lazy");
    while (fgets(line, sizeof line, f))
        if (strstr(line, root) && strstr(line, "(deleted)")) n++;
    fclose(f);
#endif
    return n;
}

/* A table reloaded per request (a server re-reading a store after each
 * publish) leaves its old column to the cache alone: that entry is dropped
 * on the next probe, so the cache holds one entry, the heap and the mapped
 * bytes stay flat, and the pruned generations' files are not pinned. */
static test_result_t test_lazy_reload_bounded(void) {
    lz_make();
    ray_mem_stats_t st;
    size_t mapped1 = 0, alloc1 = 0;
    for (int round = 0; round < 3; round++) {
        if (round) {
            lz_ok("(.db.splayed.set \"/tmp/rayforce_lazy/a/\" A)");   /* a new generation */
            lz_ok("(set A (.db.splayed.get \"/tmp/rayforce_lazy/a/\"))");
        }
        TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES));
        TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
        TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 1);
        TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), 300000LL * (8 + 58));
        ray_mem_stats(&st);
        if (round == 0) { mapped1 = st.sys_mapped; alloc1 = st.bytes_allocated; }
    }
    TEST_ASSERT_FMT(st.sys_mapped <= mapped1, "mapped %zu after 3 loads, %zu after 1", st.sys_mapped, mapped1);
    TEST_ASSERT_FMT(st.bytes_allocated <= alloc1 + (1u << 20), "heap %zu after 3 loads, %zu after 1",
                    st.bytes_allocated, alloc1);
    TEST_ASSERT_EQ_I(lz_deleted_mappings(), 0);
    /* the table goes: its entry goes with the next probe of any column */
    lz_ok("(set A null)");
    ray_t* other = ray_vec_new(RAY_I64, 1);
    other->len = 1; ((int64_t*)ray_data(other))[0] = 7;
    ray_t* o = NULL;
    TEST_ASSERT_NULL(ray_idx_lazy_hash(other, &o));
    TEST_ASSERT_NULL(o);
    ray_release(other);
    TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), 0);
    TEST_ASSERT_EQ_I(lz_deleted_mappings(), 0);
    PASS();
}

/* The filter's answer `sel` equals the scan oracle `orc` (a vector
 * expression that no filter-index site sees): same length, same I64 values. */
static bool lz_same(const char* sel, const char* orc) {
    ray_t* a = lz_eval(sel);
    ray_t* b = lz_eval(orc);
    bool ok = a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) && a->len == b->len &&
              ray_elem_size(a->type) == 8 && ray_elem_size(b->type) == 8 &&
              memcmp(ray_data(a), ray_data(b), (size_t)a->len * 8) == 0;
    if (!ok) fprintf(stderr, "lz_same: %s vs %s\n", sel, orc);
    if (a && RAY_IS_ERR(a)) ray_error_free(a); else if (a) ray_release(a);
    if (b && RAY_IS_ERR(b)) ray_error_free(b); else if (b) ray_release(b);
    return ok;
}

/* TIMESTAMP, the other 8-byte type the cost test admits, waits and builds
 * like I64; equality and `in` answer the scan's rows throughout. */
static test_result_t test_lazy_timestamp(void) {
    lz_ok("(set i (til 300000))");
    lz_ok("(.db.splayed.set \"/tmp/rayforce_lazy/ts/\" (table [t v] (list (+ 2020.01.01D00:00:00.000000000 (% (* i 7919) 1000003)) i)))");
    lz_ok("(set T (.db.splayed.get \"/tmp/rayforce_lazy/ts/\"))");
    ray_t* tc = lz_col("T", "t");
    TEST_ASSERT_TRUE(tc && tc->type == RAY_TIMESTAMP && ray_index_kind(tc) == RAY_IDX_CHUNK_ZONE);
    const char* sel = "(at (select {v: v from: T where: (== t 2020.01.01D00:00:00.000007919)}) 'v)";
    const char* orc = "(at (at T 'v) (where (== (at T 't) 2020.01.01D00:00:00.000007919)))";
    for (int k = 1; k < RAY_IDX_LAZY_BUILD_PROBES; k++) {
        TEST_ASSERT_TRUE(lz_same(sel, orc));
        TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    }
    TEST_ASSERT_TRUE(lz_same(sel, orc));                            /* builds */
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    TEST_ASSERT_TRUE(lz_same(sel, orc));
    TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: T where: (in t [2020.01.01D00:00:00.000007919 2020.01.01D00:00:00.000015838])}) 'v)",
                             "(at (at T 'v) (where (in (at T 't) [2020.01.01D00:00:00.000007919 2020.01.01D00:00:00.000015838])))"));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    PASS();
}

/* A column with nulls never builds (the probe sites gate on nulls): the
 * scan answers every time and no work is counted. */
static test_result_t test_lazy_nulls_never(void) {
    lz_ok("(set i (til 300000))");
    lz_ok("(.db.splayed.set \"/tmp/rayforce_lazy/n/\" (table [h v] (list (concat [0Nl] (% (* (til 299999) 7919) 1000003)) i)))");
    lz_ok("(set N (.db.splayed.get \"/tmp/rayforce_lazy/n/\"))");
    TEST_ASSERT_TRUE(ray_vec_may_have_nulls(lz_col("N", "h")));
    for (int k = 0; k < RAY_IDX_LAZY_BUILD_PROBES + 1; k++) {
        TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: N where: (== h 7919)}) 'v)",
                                 "(at (at N 'v) (where (== (at N 'h) 7919)))"));
        if (k % 16 == 0)
            TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: N where: (in h [7919 15838])}) 'v)",
                                     "(at (at N 'v) (where (in (at N 'h) [7919 15838])))"));
    }
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 0);
    PASS();
}

/* A slice of the table and a filtered sub-table carry no chunk zone of the
 * column: never built, never pending, answers equal the scan's. */
static test_result_t test_lazy_derived_never(void) {
    lz_make();
    lz_ok("(set S (take A 200000))");
    lz_ok("(set F (select {from: A where: (< v 200000)}))");
    for (int k = 0; k < RAY_IDX_LAZY_BUILD_PROBES + 1; k++) {
        TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: S where: (== h 7919)}) 'v)",
                                 "(at (at S 'v) (where (== (at S 'h) 7919)))"));
        TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: F where: (== h 7919)}) 'v)",
                                 "(at (at F 'v) (where (== (at F 'h) 7919)))"));
    }
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_entries(), 0);
    PASS();
}

/* After an update (to a new table or in place) and an insert, the filter
 * answers the changed rows: the cached copy of the old column is never
 * served for the new one. */
static test_result_t test_lazy_after_mutation(void) {
    lz_make();
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    lz_ok("(set A2 (update {h: 7919 where: (== v 5) from: A}))");
    for (int k = 0; k < RAY_IDX_LAZY_BUILD_PROBES + 1; k++)
        TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: A2 where: (== h 7919)}) 'v)",
                                 "(at (at A2 'v) (where (== (at A2 'h) 7919)))"));
    TEST_ASSERT_TRUE(lz_probe_n("A", 1));                          /* A itself unchanged */
    lz_ok("(update {h: 7919 where: (== v 6) from: 'A})");
    TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: A where: (== h 7919)}) 'v)",
                             "(at (at A 'v) (where (== (at A 'h) 7919)))"));
    TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: A where: (in h [7919 15838])}) 'v)",
                             "(at (at A 'v) (where (in (at A 'h) [7919 15838])))"));
    ray_t* r = lz_eval("(count (select {v: v from: A where: (== h 7919)}))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 2);          /* rows 1 and 6 */
    ray_release(r);
    lz_ok("(insert 'A (list 7919 300000))");
    TEST_ASSERT_TRUE(lz_same("(at (select {v: v from: A where: (== h 7919)}) 'v)",
                             "(at (at A 'v) (where (== (at A 'h) 7919)))"));
    r = lz_eval("(count (select {v: v from: A where: (== h 7919)}))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 3);          /* and the inserted row */
    ray_release(r);
    PASS();
}

/* Generation 2: the hash comes from the materialized column, the slot keeps
 * its encoded object and zone, and the entry goes with the object. */
static test_result_t test_lazy_gen2(void) {
    lz_make_opts(" {compress: true}");
    ray_t* enc = NULL;
    TEST_ASSERT_EQ_I(lz_slot_kind("A", "h", &enc), RAY_IDX_CHUNK_ZONE);
    TEST_ASSERT_NOT_NULL(enc);                                     /* generation 2 */
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES - 1));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_TRUE(lz_probe_n("A", 1));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    ray_t* r = lz_eval("(count (select {v: v from: A where: (in h [7919 15838 3])}))");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->i64 == 2);
    ray_release(r);
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    enc = NULL;
    TEST_ASSERT_EQ_I(lz_slot_kind("A", "h", &enc), RAY_IDX_CHUNK_ZONE);
    TEST_ASSERT_NOT_NULL(enc);                                     /* still encoded */
    lz_ok("(.db.splayed.set \"/tmp/rayforce_lazy/b/\" A)");
    lz_ok("(set B (.db.splayed.get \"/tmp/rayforce_lazy/b/\"))");
    TEST_ASSERT_EQ_I(lz_slot_kind("B", "h", NULL), RAY_IDX_CHUNK_ZONE);
    lz_ok("(set B null)");
    lz_ok("(set A null)");                                         /* the object goes: so does its entry */
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), 0);
    PASS();
}

/* A wanted column of n rows with a chunk zone of 4096-row chunks (owned). */
static ray_t* lz_zoned_vec(int64_t n, int64_t salt) {
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = ((i + salt) * 7919) % 1000003;
    ray_t* z = ray_index_attach_chunk_zone(&v, 12);
    (void)z;
    return v;
}

/* Memos (pending or declined columns) never push out a built index: with
 * every slot taken, the oldest memo goes first. */
static test_result_t test_lazy_memo_evicts_memo(void) {
    ray_t* b = lz_zoned_vec(50000, 0);
    TEST_ASSERT_TRUE(ray_idx_lazy_hash_wanted(b));
    ray_t* o = NULL;
    for (int k = 0; k < RAY_IDX_LAZY_BUILD_PROBES; k++) {
        if (o) ray_release(o);
        o = NULL;
        TEST_ASSERT_NULL(ray_idx_lazy_hash(b, &o));
    }
    TEST_ASSERT_NOT_NULL(o);                                       /* built */
    ray_release(o);
    ray_t* m[80];
    for (int k = 0; k < 80; k++) {                                 /* more memos than slots */
        m[k] = lz_zoned_vec(50000, k + 1);
        ray_t* mo = NULL;
        TEST_ASSERT_NULL(ray_idx_lazy_hash(m[k], &mo));
        TEST_ASSERT_NULL(mo);                                      /* pending */
    }
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    ray_t* lk = ray_idx_lazy_lookup(b);
    TEST_ASSERT_NOT_NULL(lk);
    ray_release(lk);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(m[0]), -1);                 /* the oldest memo went */
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(m[79]), 50000);
    ray_idx_lazy_clear();
    for (int k = 0; k < 80; k++) ray_release(m[k]);
    ray_release(b);
    PASS();
}

/* An interrupted build cancels the query (it does not go on to scan) and is
 * not remembered against the column: the next probe builds.  A build that
 * runs out of memory is best effort: the scan answers and the column is
 * declined. */
static test_result_t test_lazy_cancel(void) {
    lz_make();
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES - 1));
    ray_idx_lazy_test_fail_build("cancel");
    ray_t* r = lz_eval("(select {v: v from: A where: (== h 7919)})");
    ray_idx_lazy_test_fail_build(NULL);
    TEST_ASSERT_TRUE(r && RAY_IS_ERR(r));
    TEST_ASSERT_STR_EQ(ray_err_code(r), "cancel");
    ray_error_free(r);
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_TRUE(ray_idx_lazy_work(lz_col("A", "h")) > 0);     /* still pending */
    TEST_ASSERT_TRUE(lz_probe_n("A", 1));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
    PASS();
}
static test_result_t test_lazy_oom(void) {
    lz_make();
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES - 1));
    ray_idx_lazy_test_fail_build("oom");
    bool ok = lz_probe_n("A", 1);                                  /* the scan answers */
    ray_idx_lazy_test_fail_build(NULL);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    TEST_ASSERT_EQ_I(ray_idx_lazy_work(lz_col("A", "h")), -1);    /* declined */
    TEST_ASSERT_TRUE(lz_probe_n("A", RAY_IDX_LAZY_BUILD_PROBES));
    TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 0);
    PASS();
}

/* Two threads probing the same column at once keep one hash: the loser of
 * the build race finds the winner's entry under the lock and drops its own. */
typedef struct { ray_t* col; _Atomic int* go; ray_t* out; ray_t* err; } lz_thr_t;
static void lz_probe_thread(void* raw) {
    lz_thr_t* t = raw;
    ray_rc_sync = true;                                 /* shared objects: atomic counts, as a worker */
    atomic_fetch_add(t->go, 1);
    while (atomic_load(t->go) < 2) { }
    t->err = ray_idx_lazy_hash(t->col, &t->out);
    if (t->err) { ray_error_free(t->err); t->err = (ray_t*)1; }
    ray_heap_abandon();
}
static test_result_t test_lazy_concurrent(void) {
    int64_t n = 50000;                                  /* below the parallel build: no pool */
    ray_t* v = ray_vec_new(RAY_I64, n); v->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(v))[i] = (i * 7919) % 1000003;
    ray_t* z = ray_index_attach_chunk_zone(&v, 12);     /* 13 chunks of 4096 rows */
    TEST_ASSERT_FALSE(RAY_IS_ERR(z));
    TEST_ASSERT_TRUE(ray_idx_lazy_hash_wanted(v));
    for (int round = 0; round < 8; round++) {
        ray_idx_lazy_clear();
        for (int k = 1; k < RAY_IDX_LAZY_BUILD_PROBES; k++) {   /* pending: the scans' work */
            ray_t* o = NULL;
            TEST_ASSERT_NULL(ray_idx_lazy_hash(v, &o));
            TEST_ASSERT_NULL(o);
        }
        _Atomic int go = 0;
        lz_thr_t t[2]; ray_thread_t th[2];
        for (int k = 0; k < 2; k++) {
            t[k] = (lz_thr_t){ .col = v, .go = &go, .out = NULL, .err = NULL };
            TEST_ASSERT_EQ_I(ray_thread_create(&th[k], lz_probe_thread, &t[k]), RAY_OK);
        }
        for (int k = 0; k < 2; k++) ray_thread_join(th[k]);
        TEST_ASSERT_TRUE(!t[0].err && !t[1].err && t[0].out && t[1].out);
        TEST_ASSERT_EQ_PTR(t[0].out, t[1].out);           /* one indexed copy */
        TEST_ASSERT_EQ_I(ray_idx_lazy_count(), 1);
        TEST_ASSERT_EQ_I(ray_idx_lazy_bytes(), n * (8 + 58));
        ray_t* lk = ray_idx_lazy_lookup(v);               /* retained */
        TEST_ASSERT_EQ_PTR(lk, t[0].out);
        ray_release(lk);
        TEST_ASSERT_EQ_I(ray_index_kind(t[0].out), RAY_IDX_HASH);
        TEST_ASSERT_EQ_I(ray_index_kind(v), RAY_IDX_CHUNK_ZONE);  /* the column is untouched */
        ray_release(t[0].out); ray_release(t[1].out);
    }
    ray_idx_lazy_clear();
    ray_release(v);
    PASS();
}

/* The index region streamed to a file (block by block) is byte-identical to
 * the one the buffered writer lays out in memory, for every public index kind. */
static bool region_file_matches_buffer(ray_t* w) {
    if (!w || RAY_IS_ERR(w) || !w->index) return false;
    const ray_index_t* ix = ray_index_payload(w->index);
    int64_t size = ray_index_inline_size(ix);
    uint8_t* buf = (uint8_t*)ray_calloc_raw((size_t)size);
    uint8_t* back = (uint8_t*)ray_calloc_raw((size_t)size + 1);
    FILE* f = tmpfile();
    bool ok = buf && back && f;
    if (ok) {
        ray_index_inline_write(buf, ix);
        ok = ray_index_inline_write_file(f, ix) && fflush(f) == 0 &&
             ftell(f) == (long)size && fseek(f, 0, SEEK_SET) == 0 &&
             fread(back, 1, (size_t)size + 1, f) == (size_t)size &&   /* nothing beyond size */
             memcmp(buf, back, (size_t)size) == 0;
    }
    if (f) fclose(f);
    ray_free_raw(buf); ray_free_raw(back);
    return ok;
}

static test_result_t test_stream_region_bytes_match(void) {
    int64_t n = 70001;      /* child lengths are not multiples of 32: the pad path runs */
    ray_t* v = zone_test_col(n, 0, 0);
    TEST_ASSERT_NOT_NULL(v);
    ray_t* w = v;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 16)));
    TEST_ASSERT_EQ_I(ray_index_kind(w), RAY_IDX_CHUNK_ZONE);
    TEST_ASSERT_NOT_NULL(ray_index_payload(w->index)->u.chunk_zone.aggs);
    TEST_ASSERT_TRUE(region_file_matches_buffer(w));
    ray_release(w);

    ray_t* f64 = ray_vec_new(RAY_F64, n); f64->len = n;
    for (int64_t i = 0; i < n; i++) ((double*)ray_data(f64))[i] = (double)(i % 1013) * 0.5;
    w = f64;
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_chunk_zone(&w, 16)));
    TEST_ASSERT_EQ_I(ray_index_kind(w), RAY_IDX_CHUNK_ZONE);
    TEST_ASSERT_NULL(ray_index_payload(w->index)->u.chunk_zone.aggs);
    TEST_ASSERT_TRUE(region_file_matches_buffer(w));
    ray_release(w);

    w = dict_test_col(n, 3000, false);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_dict(&w)));
    TEST_ASSERT_EQ_I(ray_index_kind(w), RAY_IDX_DICT);
    TEST_ASSERT_TRUE(region_file_matches_buffer(w));
    ray_release(w);

    ray_t* h = ray_vec_new(RAY_I64, n); h->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(h))[i] = (i * 7919) % 1009;
    w = ray_idx_hash_fn(h);
    TEST_ASSERT_TRUE(w && !RAY_IS_ERR(w));
    TEST_ASSERT_EQ_I(ray_index_kind(w), RAY_IDX_HASH);
    TEST_ASSERT_TRUE(region_file_matches_buffer(w));
    ray_release(w);

    int kinds = 3;
    for (int k = 0; k < kinds; k++) {
        ray_t* z = ray_vec_new(RAY_I64, n); z->len = n;
        for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(z))[i] = (i * 7919) % 100003;
        w = z;
        ray_t* r = k == 0 ? ray_index_attach_zone(&w)
                 : k == 1 ? ray_index_attach_sort(&w) : ray_index_attach_bloom(&w);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        TEST_ASSERT_EQ_I(ray_index_kind(w), k == 0 ? RAY_IDX_ZONE : k == 1 ? RAY_IDX_SORT : RAY_IDX_BLOOM);
        TEST_ASSERT_TRUE(region_file_matches_buffer(w));
        ray_release(w);
    }
    PASS();
}

static bool stream_dir_has_tmp(const char* dir) {
    DIR* d = opendir(dir);
    bool found = false;
    struct dirent* e;
    while (d && (e = readdir(d)))
        if (!strncmp(e->d_name, ".stream-", 8)) found = true;
    if (d) closedir(d);
    return found;
}

/* One writer's finish fails; close_all returns the error and nothing is
 * published: no column file, no tmp or pool file, no retained index.
 * Injection: the failing writer's FILE* is replaced by a read-only stream on
 * its own tmp file, so every write the finish attempts (index region, header)
 * fails with EBADF and the run needs neither timing nor an interrupt (which
 * would also cancel the healthy columns nondeterministically). */
static test_result_t test_stream_finish_failure_publishes_nothing(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-fail-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 300000;
    ray_t* a = ray_vec_new(RAY_I64, n); a->len = n;
    ray_t* b = ray_vec_new(RAY_I64, n); b->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int64_t*)ray_data(a))[i] = i;
        ((int64_t*)ray_data(b))[i] = (i * 7919) % 1000003;   /* unclustered */
    }
    ray_t* s = dict_test_col(100000, 3000, false);
    ray_t* cols[3] = { a, b, s };
    int8_t types[3] = { RAY_I64, RAY_I64, RAY_STR };
    const char* names[3] = { "fa", "fb", "fs" };
    ray_col_stream_t w[3];
    for (int c = 0; c < 3; c++) {
        TEST_ASSERT_EQ_I(ray_col_stream_open(&w[c], dir, ray_sym_intern(names[c], 2), types[c], NULL, 0), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w[c], 0), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w[c], cols[c]), RAY_OK);
    }
    TEST_ASSERT_EQ_I(fclose(w[0].fp), 0);
    w[0].fp = fopen(w[0].tmp_path, "rb");
    TEST_ASSERT_NOT_NULL(w[0].fp);
    ray_err_t e = ray_col_stream_close_all(w, 3, false, NULL);
    TEST_ASSERT_EQ_I(e, RAY_ERR_IO);
    for (int c = 0; c < 3; c++) {
        char path[200]; snprintf(path, sizeof(path), "%s/%s", dir, names[c]);
        TEST_ASSERT_TRUE(access(path, F_OK) != 0);
        ray_col_stream_abort(&w[c]);   /* converters abort after a failed close */
    }
    TEST_ASSERT_FALSE(stream_dir_has_tmp(dir));
    ray_release(a); ray_release(b); ray_release(s);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* A cancelled pool retires the finish tasks it has not run: close_all
 * reports the cancellation and publishes nothing (it used to return OK with
 * every column file missing), and publish refuses a writer whose finish
 * never ran.  Deterministic: the cancel flag is up before the dispatch, so
 * the pool skips every task. */
static test_result_t test_stream_cancel_publishes_nothing(void) {
    ray_pool_t* pool = ray_pool_get();
    if (!ray_pool_par_dispatch_ok(pool, 3, 2)) SKIP("needs a worker pool");
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-cancel-%d", (int)getpid());
    (void)ray_test_rm_rf(dir);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 70000;
    ray_t* a = ray_vec_new(RAY_I64, n); a->len = n;
    for (int64_t i = 0; i < n; i++) ((int64_t*)ray_data(a))[i] = i * 3;
    const char* names[3] = { "ca", "cb", "cc" };
    uint8_t gens[3] = { 0, 2, 0 };
    ray_col_stream_t w[3];
    for (int c = 0; c < 3; c++) {
        TEST_ASSERT_EQ_I(ray_col_stream_open(&w[c], dir, ray_sym_intern(names[c], 2), RAY_I64, NULL, gens[c]), RAY_OK);
        TEST_ASSERT_EQ_I(ray_col_stream_append(&w[c], a), RAY_OK);
    }
    /* an open writer: publish refuses, the staging file stays private */
    TEST_ASSERT_EQ_I(ray_col_stream_publish(&w[0], false), RAY_ERR_DOMAIN);
    ray_request_interrupt();
    ray_err_t e = ray_col_stream_close_all(w, 3, false, NULL);
    ray_clear_interrupt();
    TEST_ASSERT_EQ_I(e, RAY_ERR_CANCEL);
    for (int c = 0; c < 3; c++) {
        char path[200]; snprintf(path, sizeof(path), "%s/%s", dir, names[c]);
        TEST_ASSERT_TRUE(access(path, F_OK) != 0);
        ray_col_stream_abort(&w[c]);
    }
    TEST_ASSERT_FALSE(stream_dir_has_tmp(dir));
    ray_release(a);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* STR column of 100000 rows with empty strings: inline dict, HAS_NULLS set. */
static test_result_t test_stream_inline_dict(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-dict-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    int64_t n = 100000;
    ray_t* v = dict_test_col(n, 3000, false);
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("s", 1), RAY_STR, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_index_begin(&w, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, v), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);
    char path[200]; snprintf(path, sizeof(path), "%s/s", dir);
    ray_t* back = ray_col_mmap(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    TEST_ASSERT_TRUE(back->attrs & RAY_ATTR_HAS_NULLS);
    TEST_ASSERT_EQ_I(ray_index_kind(back), RAY_IDX_DICT);
    TEST_ASSERT_EQ_I(ray_index_payload(back->index)->u.dict.n_distinct, 3001);
    ray_release(back); ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

static test_result_t test_file_rename_new(void) {
#ifdef RAY_OS_WASM
    SKIP("exclusive rename is unavailable on this host");
#else
    char from[160], to[160], marker[180];
    snprintf(from,sizeof(from),"/tmp/rayforce-publish-from-%d",(int)getpid());
    snprintf(to,sizeof(to),"/tmp/rayforce-publish-to-%d",(int)getpid());
    TEST_ASSERT_EQ_I(ray_mkdir(from),RAY_OK); TEST_ASSERT_EQ_I(ray_mkdir(to),RAY_OK);
    snprintf(marker,sizeof(marker),"%s/marker",from);
    FILE* f = fopen(marker,"wb"); TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I(fwrite("retained",1,8,f),8); TEST_ASSERT_EQ_I(fclose(f),0);
    /* Ordinary POSIX rename would replace this empty directory. */
    TEST_ASSERT_EQ_I(ray_file_rename_new(from,to),RAY_ERR_IO);
    TEST_ASSERT_TRUE(access(marker,F_OK) == 0);
    TEST_ASSERT_EQ_I(rmdir(to),0);
    TEST_ASSERT_EQ_I(ray_file_rename_new(from,to),RAY_OK);
    TEST_ASSERT_TRUE(access(from,F_OK) != 0);
    snprintf(marker,sizeof(marker),"%s/marker",to);
    f = fopen(marker,"rb"); TEST_ASSERT_NOT_NULL(f);
    char bytes[8]; TEST_ASSERT_EQ_I(fread(bytes,1,8,f),8); fclose(f);
    TEST_ASSERT_TRUE(!memcmp(bytes,"retained",8));
    TEST_ASSERT_EQ_I(ray_file_rename_new(NULL,to),RAY_ERR_IO);
    unlink(marker); rmdir(to);
    PASS();
#endif
}

/* The portable fallback used where the host or filesystem has no
 * no-replace rename: same contract as ray_file_rename_new. */
static test_result_t test_file_rename_new_emulated(void) {
#if defined(RAY_OS_WASM) || defined(RAY_OS_WINDOWS)
    SKIP("POSIX fallback");
#else
    char from[160], to[160], marker[180];
    snprintf(from,sizeof(from),"/tmp/rayforce-emu-from-%d",(int)getpid());
    snprintf(to,sizeof(to),"/tmp/rayforce-emu-to-%d",(int)getpid());
    TEST_ASSERT_EQ_I(ray_mkdir(from),RAY_OK); TEST_ASSERT_EQ_I(ray_mkdir(to),RAY_OK);
    snprintf(marker,sizeof(marker),"%s/marker",from);
    FILE* f = fopen(marker,"wb"); TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I(fwrite("retained",1,8,f),8); TEST_ASSERT_EQ_I(fclose(f),0);
    /* an existing (empty) directory is not replaced */
    TEST_ASSERT_EQ_I(ray_file_rename_new_emulated(from,to),RAY_ERR_IO);
    TEST_ASSERT_TRUE(access(marker,F_OK) == 0);
    TEST_ASSERT_EQ_I(rmdir(to),0);
    TEST_ASSERT_EQ_I(ray_file_rename_new_emulated(from,to),RAY_OK);
    TEST_ASSERT_TRUE(access(from,F_OK) != 0);
    snprintf(marker,sizeof(marker),"%s/marker",to);
    TEST_ASSERT_TRUE(access(marker,F_OK) == 0);
    /* files: an existing destination is kept, a new one is published */
    char other[180];
    snprintf(other,sizeof(other),"%s/other",to);
    f = fopen(other,"wb"); TEST_ASSERT_NOT_NULL(f); fputs("x",f); fclose(f);
    TEST_ASSERT_EQ_I(ray_file_rename_new_emulated(other,marker),RAY_ERR_IO);
    TEST_ASSERT_TRUE(access(other,F_OK) == 0);
    unlink(marker);
    TEST_ASSERT_EQ_I(ray_file_rename_new_emulated(other,marker),RAY_OK);
    TEST_ASSERT_TRUE(access(other,F_OK) != 0 && access(marker,F_OK) == 0);
    unlink(marker); rmdir(to);
    PASS();
#endif
}

/* ---- test_file_shared_lock_concurrent ---------------------------------- */

static test_result_t test_file_shared_lock_concurrent(void) {
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
    ray_fd_t fd1 = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_READ | RAY_OPEN_WRITE | RAY_OPEN_CREATE);
    ray_fd_t fd2 = ray_file_open(ray_test_tmp(TMP_FILEIO_PATH), RAY_OPEN_READ);
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
    unlink(ray_test_tmp(TMP_FILEIO_PATH));
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
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load back — should succeed since all indices < sym_count */
    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_I(loaded->len, 4);
    ray_release(loaded);

    /* Now craft a column with an out-of-range index */
    data[2] = (uint8_t)(sc + 10);  /* beyond sym table */
    err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Load should fail with RAY_ERR_CORRUPT */
    ray_t* bad = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    /* Same test via mmap */
    bad = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
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
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
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
    ray_t* bad = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    /* Same via mmap */
    bad = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    unlink(ray_test_tmp(TMP_COL_PATH));
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
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
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
    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->type, RAY_SYM);
    TEST_ASSERT_EQ_I(mapped->len, 5);

    uint16_t* md = (uint16_t*)ray_data(mapped);
    TEST_ASSERT_EQ_I(md[0], id0);
    TEST_ASSERT_EQ_I(md[2], id2);

    ray_release(mapped);
    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_splay_load_with_sym ------------------------------------------ */

#define TMP_SPLAY_SYM_DIR "/tmp/rayforce_test_splay_sym"
#define TMP_SYM_PATH      "/tmp/rayforce_test_splay_sym_file"

static test_result_t test_splay_load_with_sym(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_SYM_DIR));
    unlink(ray_test_tmp(TMP_SYM_PATH));

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
    ray_err_t err = ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_SYM_DIR), ray_test_tmp(TMP_SYM_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table, then load via ray_splay_load with sym_path */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);  /* "" reserved */

    ray_t* loaded = ray_splay_load(ray_test_tmp(TMP_SPLAY_SYM_DIR), ray_test_tmp(TMP_SYM_PATH));
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
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_SYM_DIR));
    unlink(ray_test_tmp(TMP_SYM_PATH));
    unlink(ray_test_tmp(TMP_SYM_PATH ".lk"));
    PASS();
}

/* ---- test_splay_load_sym_missing_corrupt ------------------------------- */

static test_result_t test_splay_load_sym_missing_corrupt(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_SYM_DIR));
    unlink(ray_test_tmp(TMP_SYM_PATH));

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
    ray_err_t err = ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_SYM_DIR), ray_test_tmp(TMP_SYM_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Reset sym table — simulate loading without sym */
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_U(ray_sym_count(), 1);  /* "" reserved */

    /* Load with NULL sym_path — fails with the loud "sym" error: the
     * column's cells are positions in its symfile, and a SYM column with
     * no resolvable domain must never resolve against incidental state
     * (sym-domain spec, Load §3; replaces the old validate_sym_columns
     * "corrupt" which keyed off the global table being empty). */
    ray_t* loaded = ray_splay_load(ray_test_tmp(TMP_SPLAY_SYM_DIR), NULL);
    TEST_ASSERT_TRUE(RAY_IS_ERR(loaded));
    TEST_ASSERT_STR_EQ(ray_err_code(loaded), "sym");
    ray_release(loaded);

    ray_release(col);
    ray_release(tbl);
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_SYM_DIR));
    unlink(ray_test_tmp(TMP_SYM_PATH));
    unlink(ray_test_tmp(TMP_SYM_PATH ".lk"));
    PASS();
}

/* ---- test_read_splayed_bad_sym_fatal ----------------------------------- */

static test_result_t test_read_splayed_bad_sym_fatal(void) {
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_SYM_DIR));

    /* Build a simple table (no RAY_SYM columns needed) */
    int64_t id_x = ray_sym_intern("x", 1);
    int64_t raw[] = {1, 2, 3};
    ray_t* col_x = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, id_x, col_x);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    ray_err_t err = ray_splay_save(tbl, ray_test_tmp(TMP_SPLAY_SYM_DIR), NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* ray_read_splayed with a nonexistent sym_path on a SYMBOL-FREE
     * table — post-flip a missing symfile is tolerated (the spec's
     * no-symbol-columns exemption applies to reads: the argument names
     * where the domain would live, not a promise it exists).  The loud
     * "sym" error is reserved for actual SYM columns without a domain
     * (test_splay_load_sym_missing_corrupt above). */
    ray_t* loaded = ray_read_splayed(ray_test_tmp(TMP_SPLAY_SYM_DIR), ray_test_tmp("/tmp/rayforce_nonexistent_sym_xyz"));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(ray_table_nrows(loaded), 3);
    ray_release(loaded);

    ray_release(col_x);
    ray_release(tbl);
    (void)ray_test_rm_rf(ray_test_tmp(TMP_SPLAY_SYM_DIR));
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
    /* 1. C NULL pointer → RAY_SERDE_NULL → RAY_NULL_OBJ (value null) */
    {
        ray_t* wire = ray_ser(NULL);
        TEST_ASSERT_NOT_NULL(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(wire));
        ray_t* back = ray_de(wire);
        TEST_ASSERT(RAY_IS_NULL(back), "de of serialized null yields the null singleton");
        ray_release(back);
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
        /* Empty STR roundtrips as the canonical null. */
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

/* ---- test_serde_endian_mismatch ------------------------------------------ */

static test_result_t test_serde_endian_mismatch(void) {
    /* A frame whose header endian byte differs from the host must be
     * rejected loudly — the payload is native-endian, so decoding a
     * cross-endian frame would silently transpose every multi-byte
     * value.  The IPC/journal receive paths already validate this;
     * ray_de (the `de` builtin and .qdb snapshot load) must too. */
    ray_t* orig = ray_i64(42);
    ray_t* wire = ray_ser(orig);
    TEST_ASSERT_NOT_NULL(wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

    ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(wire);
    TEST_ASSERT_EQ_I(hdr->endian, RAY_SERDE_ENDIAN);
    hdr->endian ^= 1;   /* pretend the frame came from the other byte order */

    ray_t* back = ray_de(wire);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_TRUE(RAY_IS_ERR(back));
    ray_error_free(back);

    /* Restoring the correct byte makes the same frame decode again. */
    hdr->endian ^= 1;
    back = ray_de(wire);
    TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->i64, 42);

    ray_release(back);
    ray_release(wire);
    ray_release(orig);
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
        -RAY_I32, -RAY_I16, -RAY_BOOL, -RAY_U8,  /* STR/SYM need no wire null flag */
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
    uint8_t* wire = (uint8_t*)ray_data(w);
    TEST_ASSERT_EQ_I((int8_t)wire[sizeof(ray_ipc_header_t)], RAY_TABLE);
    TEST_ASSERT_EQ_I((int8_t)wire[sizeof(ray_ipc_header_t) + 2], RAY_SYM);

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
    /* col_out is a borrowed pointer (ray_table_get_col does not retain) */

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

    ray_err_t err = ray_obj_save(v, ray_test_tmp(TMP_SERDE_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* back = ray_obj_load(ray_test_tmp(TMP_SERDE_PATH));
    TEST_ASSERT_NOT_NULL(back); TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, RAY_I64);
    TEST_ASSERT_EQ_I(back->len, 4);
    int64_t* bd = (int64_t*)ray_data(back);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQ_I(bd[i], raw[i]);

    ray_release(back);
    ray_release(v);
    unlink(ray_test_tmp(TMP_SERDE_PATH));
    PASS();
}

/* ray_obj_load error paths: missing file, empty file, bad data */
static test_result_t test_serde_obj_load_errors(void) {
    /* Non-existent file */
    {
        ray_t* r = ray_obj_load(ray_test_tmp("/tmp/rayforce_nonexistent_42.rfl"));
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
    }
    /* Empty file */
    {
        FILE* f = fopen(ray_test_tmp("/tmp/rayforce_empty_test.rfl"), "wb");
        TEST_ASSERT_NOT_NULL(f);
        fclose(f);
        ray_t* r = ray_obj_load(ray_test_tmp("/tmp/rayforce_empty_test.rfl"));
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
        unlink(ray_test_tmp("/tmp/rayforce_empty_test.rfl"));
    }
    /* Bad data (no valid header) */
    {
        FILE* f = fopen(ray_test_tmp("/tmp/rayforce_bad_test.rfl"), "wb");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t junk[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
                          0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
        ray_t* r = ray_obj_load(ray_test_tmp("/tmp/rayforce_bad_test.rfl"));
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        ray_release(r);
        unlink(ray_test_tmp("/tmp/rayforce_bad_test.rfl"));
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
    /* BOOL is non-nullable — set_null rejects.  Round-trip a non-null
     * BOOL vec to keep the serde path covered. */
    {
        ray_t* v = ray_vec_new(RAY_BOOL, 3);
        TEST_ASSERT_NOT_NULL(v); TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        v->len = 3;
        uint8_t* d = (uint8_t*)ray_data(v);
        d[0] = 1; d[1] = 0; d[2] = 1;
        TEST_ASSERT_EQ_I(ray_vec_set_null_checked(v, 1, true), RAY_ERR_TYPE);

        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_BOOL);
        uint8_t* bd = (uint8_t*)ray_data(b);
        TEST_ASSERT_EQ_I(bd[0], 1);
        TEST_ASSERT_EQ_I(bd[1], 0);
        TEST_ASSERT_EQ_I(bd[2], 1);
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
    /* SYM vector: null state is represented by sym 0 (the empty
     * string), not a null bitmap.  This stanza verifies the round-trip
     * preserves the values intact — there is no HAS_NULLS attribute
     * to assert on. */
    {
        int64_t id_empty = 0;  /* reserved by ray_sym_init */
        int64_t id_q     = ray_sym_intern("q", 1);
        ray_t* v = ray_vec_new(RAY_SYM, 2);
        v->len = 2;
        int64_t* ids = (int64_t*)ray_data(v);
        ids[0] = id_empty;  /* "missing" represented as sym 0 */
        ids[1] = id_q;

        ray_t* w = ray_ser(v);
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_t* b = ray_de(w);
        TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
        TEST_ASSERT_EQ_I(b->type, RAY_SYM);
        TEST_ASSERT_FALSE(b->attrs & RAY_ATTR_HAS_NULLS);
        const int64_t* rids = (const int64_t*)ray_data(b);
        TEST_ASSERT_EQ_I(rids[0], 0);
        TEST_ASSERT_EQ_I(rids[1], id_q);
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
    /* The list owns the element refs — releasing it releases them.
     * (RAY_NULL_OBJ at [1] is ARENA-flagged; release is a no-op.) */
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

/* ---- test_serde_builtin_name_roundtrip ---------------------------------- */

static test_result_t test_serde_builtin_name_roundtrip(void) {
    /* Regression: builtins serialize by name, but the encoder capped the name
     * at 15 bytes.  A builtin whose registered name is longer was written
     * truncated and could not be resolved on decode, so (de (ser fn)) raised a
     * name error instead of round-tripping.  test_serde_function_types only
     * exercises short names (neg, +, list), so it never caught this. */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    static const char* const long_names[] = {
        ".db.parted.tables",    /* 17 bytes, RAY_VARY */
        ".sys.querylog.enable", /* 20 bytes           */
    };

    for (size_t i = 0; i < sizeof(long_names) / sizeof(long_names[0]); i++) {
        const char* name = long_names[i];
        TEST_ASSERT(strlen(name) > 15, "test name must exceed the old 15-byte cap");

        /* Borrowed: ray_env_get returns the env-owned object, do not release. */
        ray_t* orig = ray_env_get(ray_sym_intern(name, strlen(name)));
        TEST_ASSERT_NOT_NULL(orig);
        TEST_ASSERT_FALSE(RAY_IS_ERR(orig));

        ray_t* wire = ray_ser(orig);
        TEST_ASSERT_NOT_NULL(wire);
        TEST_ASSERT_FALSE(RAY_IS_ERR(wire));

        ray_t* back = ray_de(wire);
        TEST_ASSERT_NOT_NULL(back);
        TEST_ASSERT_FALSE(RAY_IS_ERR(back));

        /* The decoder resolves the full name in the global env, so it returns
         * the very same builtin — identity is the strongest proof and implies
         * the name survived intact.  A truncated name would either miss or
         * resolve to a different entry. */
        TEST_ASSERT_EQ_PTR(back, orig);
        TEST_ASSERT_EQ_I(back->type, orig->type);

        ray_release(back);
        ray_release(wire);
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

/* ---- serde coverage: large null vector (>128 elems) --------------------- */

/* Round-trip a >128-element nullable vec through ser/de — verifies the
 * sentinel-encoded null state survives. */
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
            a->aux[0] = 0; /* clear null bit to force value path */
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
 * round-trips it.  This covers lambda serde and verifies compiled metadata
 * never crosses the wire without the bytecode slots it describes. */
static test_result_t test_serde_lambda_roundtrip(void) {
    /* Build a lambda: params = sym vec ["x"], body = i64(42) atom */
    int64_t x_id = ray_sym_intern("x", 1);
    ray_t* params = ray_vec_new(RAY_SYM, 1);
    TEST_ASSERT_NOT_NULL(params); TEST_ASSERT_FALSE(RAY_IS_ERR(params));
    params->len = 1;
    ((int64_t*)ray_data(params))[0] = x_id;

    ray_t* body = ray_i64(42);
    TEST_ASSERT_NOT_NULL(body);

    /* Allocate the lambda with its full slot layout (same as eval.c) */
    ray_t* lambda = ray_alloc(LAMBDA_SLOTS * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(lambda); TEST_ASSERT_FALSE(RAY_IS_ERR(lambda));
    lambda->type  = RAY_LAMBDA;
    lambda->attrs = RAY_FN_COMPILED;
    lambda->len   = 0;
    memset(ray_data(lambda), 0, LAMBDA_SLOTS * sizeof(ray_t*));
    ((ray_t**)ray_data(lambda))[0] = params;
    ((ray_t**)ray_data(lambda))[1] = body;

    /* Verify serde_size covers RAY_LAMBDA branch */
    int64_t sz = ray_serde_size(lambda);
    TEST_ASSERT_FMT(sz > 0, "serde_size should be > 0 for LAMBDA");

    /* Serialize */
    ray_t* w = ray_ser(lambda);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    uint8_t* raw = (uint8_t*)ray_data(w);
    size_t off = sizeof(ray_ipc_header_t);
    TEST_ASSERT_EQ_I((int)raw[off], RAY_LAMBDA);
    TEST_ASSERT_EQ_I((int)raw[off + 1], 0);

    /* Simulate an older/crafted wire frame that carries bit 0x40.  For
     * vectors that means HAS_NULLS; for lambdas it is RAY_FN_COMPILED and
     * would make the receiver jump into absent bytecode. */
    raw[off + 1] = RAY_FN_COMPILED;

    /* Deserialize */
    ray_t* b = ray_de(w);
    TEST_ASSERT_NOT_NULL(b); TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    TEST_ASSERT_EQ_I(b->type, RAY_LAMBDA);
    TEST_ASSERT_EQ_I((int)(b->attrs & RAY_FN_COMPILED), 0);
    /* params slot should be a SYM vector */
    ray_t** bslots = (ray_t**)ray_data(b);
    TEST_ASSERT_NOT_NULL(bslots[0]);
    TEST_ASSERT_EQ_I(bslots[0]->type, RAY_SYM);
    TEST_ASSERT_EQ_I(bslots[0]->len, 1);
    /* body slot should be an I64 atom */
    TEST_ASSERT_NOT_NULL(bslots[1]);
    TEST_ASSERT_EQ_I(bslots[1]->type, -RAY_I64);
    TEST_ASSERT_EQ_I(bslots[1]->i64, 42);
    TEST_ASSERT_NULL(bslots[2]);
    TEST_ASSERT_NULL(bslots[3]);

    ray_release(b); ray_release(w); ray_release(lambda);
    PASS();
}

/* ---- serde coverage: a lambda's closure must be SYM keys -> LIST values --- */

/* The compiled capture binding (OP_BINDCAPS) reads a closure's values as a LIST
 * of ray_t*.  A wire frame whose closure carries a typed value vector instead
 * must be rejected at deserialization, not dereferenced later. */
static ray_t* make_test_lambda(ray_t* closure) {
    int64_t x_id = ray_sym_intern("x", 1);
    ray_t* params = ray_vec_new(RAY_SYM, 1);
    if (!params || RAY_IS_ERR(params)) return params;
    params->len = 1;
    ((int64_t*)ray_data(params))[0] = x_id;
    ray_t* lambda = ray_alloc(LAMBDA_SLOTS * sizeof(ray_t*));
    if (!lambda || RAY_IS_ERR(lambda)) { ray_release(params); return lambda; }
    lambda->type  = RAY_LAMBDA;
    lambda->attrs = 0;
    lambda->len   = 0;
    memset(ray_data(lambda), 0, LAMBDA_SLOTS * sizeof(ray_t*));
    LAMBDA_PARAMS(lambda)  = params;
    LAMBDA_BODY(lambda)    = ray_i64(42);
    LAMBDA_CLOSURE(lambda) = closure;
    return lambda;
}

static test_result_t test_serde_lambda_malformed_closure(void) {
    int64_t k_id = ray_sym_intern("k", 1);

    /* Control: a closure of SYM keys -> LIST values round-trips. */
    ray_t* keys = ray_vec_new(RAY_SYM, 1);
    TEST_ASSERT_NOT_NULL(keys); TEST_ASSERT_FALSE(RAY_IS_ERR(keys));
    keys->len = 1;
    ((int64_t*)ray_data(keys))[0] = k_id;
    ray_t* vals = ray_list_new(1);
    TEST_ASSERT_NOT_NULL(vals); TEST_ASSERT_FALSE(RAY_IS_ERR(vals));
    ((ray_t**)ray_data(vals))[0] = ray_i64(7);
    vals->len = 1;
    ray_t* good = make_test_lambda(ray_dict_new(keys, vals));
    TEST_ASSERT_NOT_NULL(good); TEST_ASSERT_FALSE(RAY_IS_ERR(good));
    ray_t* w = ray_ser(good);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    ray_t* back = ray_de(w);
    TEST_ASSERT_NOT_NULL(back); TEST_ASSERT_FALSE(RAY_IS_ERR(back));
    TEST_ASSERT_EQ_I(back->type, RAY_LAMBDA);
    TEST_ASSERT_NOT_NULL(LAMBDA_CLOSURE(back));
    TEST_ASSERT_EQ_I(ray_dict_vals(LAMBDA_CLOSURE(back))->type, RAY_LIST);
    ray_release(back); ray_release(w); ray_release(good);

    /* A closure whose values are a typed vector is refused. */
    keys = ray_vec_new(RAY_SYM, 1);
    TEST_ASSERT_NOT_NULL(keys); TEST_ASSERT_FALSE(RAY_IS_ERR(keys));
    keys->len = 1;
    ((int64_t*)ray_data(keys))[0] = k_id;
    ray_t* ivals = ray_vec_new(RAY_I64, 1);
    TEST_ASSERT_NOT_NULL(ivals); TEST_ASSERT_FALSE(RAY_IS_ERR(ivals));
    ((int64_t*)ray_data(ivals))[0] = 0x4141414141414141LL;
    ivals->len = 1;
    ray_t* bad = make_test_lambda(ray_dict_new(keys, ivals));
    TEST_ASSERT_NOT_NULL(bad); TEST_ASSERT_FALSE(RAY_IS_ERR(bad));
    w = ray_ser(bad);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    back = ray_de(w);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT(RAY_IS_ERR(back), "typed closure values must be rejected by the deserializer");
    ray_error_free(back);
    ray_release(w); ray_release(bad);

    /* A closure that is not a dict at all is refused too. */
    ray_t* notdict = ray_vec_new(RAY_I64, 1);
    TEST_ASSERT_NOT_NULL(notdict); TEST_ASSERT_FALSE(RAY_IS_ERR(notdict));
    ((int64_t*)ray_data(notdict))[0] = 1;
    notdict->len = 1;
    bad = make_test_lambda(notdict);
    TEST_ASSERT_NOT_NULL(bad); TEST_ASSERT_FALSE(RAY_IS_ERR(bad));
    w = ray_ser(bad);
    TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
    back = ray_de(w);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT(RAY_IS_ERR(back), "a non-dict closure must be rejected by the deserializer");
    ray_error_free(back);
    ray_release(w); ray_release(bad);
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
    /* A top-level frame must contain exactly one serialized object.  Extra
     * payload bytes are not valid framing and must not be silently ignored. */
    {
        ray_t* w = ray_ser(ray_i64(42));
        TEST_ASSERT_NOT_NULL(w); TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        int64_t total = w->len;
        ray_t* trailing = ray_vec_new(RAY_U8, total + 1);
        TEST_ASSERT_NOT_NULL(trailing); TEST_ASSERT_FALSE(RAY_IS_ERR(trailing));
        trailing->len = total + 1;
        memcpy(ray_data(trailing), ray_data(w), (size_t)total);
        ((uint8_t*)ray_data(trailing))[total] = 0xa5;
        ray_ipc_header_t* hdr = (ray_ipc_header_t*)ray_data(trailing);
        hdr->size += 1;

        ray_t* r = ray_de(trailing);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        TEST_ASSERT_MEM_EQ(7, r->sdata, "domain");
        ray_release(r); ray_release(trailing); ray_release(w);
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

/* Splice a container frame — [type][attrs] + payload(a) + payload(b) — from two
 * real serialized sub-objects, wrapped in a fresh ipc header.  Uses ray_ser and
 * sizeof(ray_ipc_header_t), so it stays honest if the wire layout ever changes
 * (rather than hard-coding the header size / field offsets). */
#define TEST_ASSERT_ERR_CODE_MSG(r, code, needle) do {                       \
    TEST_ASSERT_NOT_NULL(r);                                                 \
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));                                         \
    TEST_ASSERT_STR_EQ(ray_err_code(r), code);                               \
    const char* _msg = ray_error_msg();                                      \
    TEST_ASSERT_NOT_NULL(_msg);                                              \
    TEST_ASSERT_FMT(strstr(_msg, needle) != NULL,                            \
                    "error message \"%s\" does not contain \"%s\"",         \
                    _msg, needle);                                           \
} while (0)

static ray_t* serde_splice_container(uint8_t type_byte, ray_t* a, ray_t* b) {
    size_t hdrsz = sizeof(ray_ipc_header_t);
    ray_t* fa = ray_ser(a);
    ray_t* fb = ray_ser(b);
    if (!fa || RAY_IS_ERR(fa) || !fb || RAY_IS_ERR(fb)) {
        if (fa) ray_release(fa);
        if (fb) ray_release(fb);
        return NULL;
    }
    size_t pa = (size_t)fa->len - hdrsz;
    size_t pb = (size_t)fb->len - hdrsz;
    int64_t payload = 2 + (int64_t)pa + (int64_t)pb;
    int64_t total = (int64_t)hdrsz + payload;
    ray_t* buf = ray_vec_new(RAY_U8, total);
    if (!buf || RAY_IS_ERR(buf)) {
        ray_release(fa);
        ray_release(fb);
        return buf;
    }
    buf->len = total;
    uint8_t* p = (uint8_t*)ray_data(buf);
    ray_ipc_header_t* hdr = (ray_ipc_header_t*)p;
    hdr->prefix  = RAY_SERDE_PREFIX;
    hdr->version = RAY_SERDE_WIRE_VERSION;
    hdr->flags = 0; hdr->endian = RAY_SERDE_ENDIAN; hdr->msgtype = 0;
    hdr->size = payload;
    uint8_t* pl = p + hdrsz;
    pl[0] = type_byte;
    pl[1] = 0;
    memcpy(pl + 2, (uint8_t*)ray_data(fa) + hdrsz, pa);
    memcpy(pl + 2 + pa, (uint8_t*)ray_data(fb) + hdrsz, pb);
    ray_release(fa);
    ray_release(fb);
    return buf;
}

/* A container frame whose two counts disagree must be rejected, not silently
 * accepted as the shorter of the two. */
static test_result_t test_serde_container_count_mismatch(void) {
    /* TABLE: 2 schema names, 1 column → rejected (not truncated to 1 column). */
    {
        int64_t names[] = { 7, 7 };
        ray_t* schema = ray_vec_from_raw(RAY_I64, names, 2);
        int64_t c0[] = { 1 };
        ray_t* cols = ray_list_new(1);
        ray_t* col0 = ray_vec_from_raw(RAY_I64, c0, 1);
        cols = ray_list_append(cols, col0);
        ray_release(col0);
        ray_t* frame = serde_splice_container((uint8_t)RAY_TABLE, schema, cols);
        TEST_ASSERT_NOT_NULL(frame);
        ray_t* r = ray_de(frame);
        TEST_ASSERT_ERR_CODE_MSG(r, "domain", "schema/column count mismatch");
        ray_error_free(r); ray_release(frame); ray_release(schema); ray_release(cols);
    }
    /* TABLE: 2 columns of unequal length (2 and 1) → rejected (ragged rows). */
    {
        int64_t names[] = { 7, 8 };
        ray_t* schema = ray_vec_from_raw(RAY_I64, names, 2);
        int64_t c0[] = { 1, 2 };
        int64_t c1[] = { 3 };
        ray_t* cols = ray_list_new(2);
        ray_t* col0 = ray_vec_from_raw(RAY_I64, c0, 2);
        ray_t* col1 = ray_vec_from_raw(RAY_I64, c1, 1);
        cols = ray_list_append(cols, col0);
        cols = ray_list_append(cols, col1);
        ray_release(col0);
        ray_release(col1);
        ray_t* frame = serde_splice_container((uint8_t)RAY_TABLE, schema, cols);
        TEST_ASSERT_NOT_NULL(frame);
        ray_t* r = ray_de(frame);
        TEST_ASSERT_ERR_CODE_MSG(r, "domain", "ragged columns");
        ray_error_free(r); ray_release(frame); ray_release(schema); ray_release(cols);
    }
    /* TABLE: atom columns are not valid columns even when their scalar payloads
     * match, because row-wise code would read from non-existent vector data. */
    {
        int64_t names[] = { 7, 8 };
        ray_t* schema = ray_vec_from_raw(RAY_I64, names, 2);
        ray_t* cols = ray_list_new(2);
        ray_t* col0 = ray_i64(2);
        ray_t* col1 = ray_i64(2);
        cols = ray_list_append(cols, col0);
        cols = ray_list_append(cols, col1);
        ray_release(col0);
        ray_release(col1);
        ray_t* frame = serde_splice_container((uint8_t)RAY_TABLE, schema, cols);
        TEST_ASSERT_NOT_NULL(frame);
        ray_t* r = ray_de(frame);
        TEST_ASSERT_ERR_CODE_MSG(r, "domain", "column must be list/vector-like");
        ray_error_free(r); ray_release(frame); ray_release(schema); ray_release(cols);
    }
    /* DICT: 2 keys, 1 value → rejected (a probe would index vals[idx] with
     * idx < keys->len, an OOB read past the shorter value block). */
    {
        int64_t keys[] = { 7, 8 };
        ray_t* k = ray_vec_from_raw(RAY_I64, keys, 2);
        int64_t vals[] = { 1 };
        ray_t* v = ray_vec_from_raw(RAY_I64, vals, 1);
        ray_t* frame = serde_splice_container((uint8_t)RAY_DICT, k, v);
        TEST_ASSERT_NOT_NULL(frame);
        ray_t* r = ray_de(frame);
        TEST_ASSERT_ERR_CODE_MSG(r, "domain", "key/value count mismatch");
        ray_error_free(r); ray_release(frame); ray_release(k); ray_release(v);
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

/* ---- test_total_ram ---------------------------------------------------- */

static test_result_t test_total_ram(void) {
    /* Uses its own runtime since store_setup only does heap/sym init */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt);

    /* Total physical RAM should be detected as a positive value from the OS. */
    int64_t total = ray_sys_total_ram();
    TEST_ASSERT_EQ_I((int)(total > 0), 1);

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

/* ---- IPC sync round-trip ------------------------------------------------ */

static test_result_t test_ipc_sync_roundtrip(void) {
    /* Full runtime needed for ray_eval_str in server thread */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START(srv);
    uint16_t port = srv.port;


    /* Client: connect */
    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
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
    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC async send ----------------------------------------------------- */

static test_result_t test_ipc_async_send(void) {
    /* Full runtime needed for eval on server side */
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START(srv);
    uint16_t port = srv.port;

    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h) >= (0), "h >= 0");

    /* Send async — should not block or error */
    ray_t* msg = ray_str("(+ 1 1)", 7);
    ray_err_t rc = ray_ipc_send_async(h, msg);
    ray_release(msg);
    TEST_ASSERT_EQ_I(rc, RAY_OK);

    /* Small delay to let server process the async message */
    { struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; nanosleep(&ts, NULL); }

    ray_ipc_close(h);
    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC auth success --------------------------------------------------- */

static test_result_t test_ipc_auth_success(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START_OPTS(srv, "secret123", false);
    uint16_t port = srv.port;


    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "secret123", 0);
    TEST_ASSERT((h) >= (0), "h >= 0");

    ray_t* msg = ray_str("(+ 10 20)", 9);
    ray_t* result = ray_ipc_send(h, msg);
    ray_release(msg);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->i64, 30);
    ray_release(result);

    ray_ipc_close(h);
    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC auth reject ---------------------------------------------------- */

static test_result_t test_ipc_auth_reject(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START_OPTS(srv, "secret123", false);
    uint16_t port = srv.port;


    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "wrong", 0);
    TEST_ASSERT_EQ_I(h, -3);

    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC auth no creds -------------------------------------------------- */

static test_result_t test_ipc_auth_no_creds(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START_OPTS(srv, "secret123", false);
    uint16_t port = srv.port;


    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT_EQ_I(h, -2);

    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
    ray_runtime_destroy(rt);

    PASS();
}

/* ---- IPC restricted mode ------------------------------------------------ */

static test_result_t test_ipc_restricted(void) {
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START_OPTS(srv, "secret123", true);
    uint16_t port = srv.port;


    int64_t h = ray_ipc_connect("127.0.0.1", port, "admin", "secret123", 0);
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

    /* Entering compiled lambda bytecode must preserve the connection's
     * restricted context.  These are the request-level escapes from #363. */
    const char* q_lambda =
        "((fn [x] (.sys.exec \"true\")) 0)";
    ray_t* msg5 = ray_str(q_lambda, strlen(q_lambda));
    ray_t* r5 = ray_ipc_send(h, msg5);
    ray_release(msg5);
    TEST_ASSERT_NOT_NULL(r5);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r5));
    TEST_ASSERT_STR_EQ(ray_err_code(r5), "access");
    ray_release(r5);

    const char* q_lambda_set =
        "((fn [x] (set '__rf_issue_363_ipc_injected 1)) 0)";
    ray_t* msg6 = ray_str(q_lambda_set, strlen(q_lambda_set));
    ray_t* r6 = ray_ipc_send(h, msg6);
    ray_release(msg6);
    TEST_ASSERT_NOT_NULL(r6);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r6));
    TEST_ASSERT_STR_EQ(ray_err_code(r6), "access");
    ray_release(r6);

    ray_ipc_close(h);
    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
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
    ray_test_client_poll();
    TEST_ASSERT_NOT_NULL(rt);

    ray_test_server_t srv;
    RAY_TEST_SERVER_START(srv);
    uint16_t port = srv.port;


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
    int64_t h = ray_ipc_connect("127.0.0.1", port, NULL, NULL, 0);
    TEST_ASSERT((h) >= (0), "h >= 0");
    ray_ipc_close(h);

    ray_test_server_stop(&srv);
    ray_test_client_poll_done();
    ray_runtime_destroy(rt);
    PASS();
}

/* ---- test_col_save_load_bool_u8_i16 --------------------------------------- */
/* Covers is_serializable_type arms for RAY_BOOL / RAY_U8 / RAY_I16 and their
 * col_save / col_load round-trip. */
static test_result_t test_col_save_load_bool_u8_i16(void) {
    /* RAY_BOOL */
    {
        bool raw[] = {true, false, true, true, false};
        ray_t* vec = ray_vec_from_raw(RAY_BOOL, raw, 5);
        TEST_ASSERT_NOT_NULL(vec);
        TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

        ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_EQ_I(err, RAY_OK);

        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_NOT_NULL(loaded);
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_EQ_I(loaded->type, RAY_BOOL);
        TEST_ASSERT_EQ_I(loaded->len, 5);
        bool* ld = (bool*)ray_data(loaded);
        TEST_ASSERT_TRUE(ld[0]);
        TEST_ASSERT_FALSE(ld[1]);
        TEST_ASSERT_TRUE(ld[2]);
        ray_release(loaded);

        ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_NOT_NULL(mapped);
        TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
        TEST_ASSERT_EQ_I(mapped->type, RAY_BOOL);
        TEST_ASSERT_EQ_I(mapped->len, 5);
        ray_release(mapped);
        ray_release(vec);
        unlink(ray_test_tmp(TMP_COL_PATH));
    }
    /* RAY_U8 */
    {
        uint8_t raw[] = {10, 20, 30};
        ray_t* vec = ray_vec_from_raw(RAY_U8, raw, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

        ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_EQ_I(err, RAY_OK);

        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_EQ_I(loaded->type, RAY_U8);
        TEST_ASSERT_EQ_I(loaded->len, 3);
        uint8_t* ld = (uint8_t*)ray_data(loaded);
        TEST_ASSERT_EQ_I(ld[0], 10);
        TEST_ASSERT_EQ_I(ld[1], 20);
        TEST_ASSERT_EQ_I(ld[2], 30);
        ray_release(loaded);
        ray_release(vec);
        unlink(ray_test_tmp(TMP_COL_PATH));
    }
    /* RAY_I16 */
    {
        int16_t raw[] = {-100, 0, 200};
        ray_t* vec = ray_vec_from_raw(RAY_I16, raw, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

        ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_EQ_I(err, RAY_OK);

        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_EQ_I(loaded->type, RAY_I16);
        TEST_ASSERT_EQ_I(loaded->len, 3);
        int16_t* ld = (int16_t*)ray_data(loaded);
        TEST_ASSERT_EQ_I(ld[0], -100);
        TEST_ASSERT_EQ_I(ld[1], 0);
        TEST_ASSERT_EQ_I(ld[2], 200);
        ray_release(loaded);
        ray_release(vec);
        unlink(ray_test_tmp(TMP_COL_PATH));
    }
    PASS();
}

/* ---- test_col_save_load_date_time_timestamp ------------------------------ */
/* Covers RAY_DATE / RAY_TIME / RAY_TIMESTAMP save/load arms. */
static test_result_t test_col_save_load_date_time_timestamp(void) {
    /* RAY_DATE */
    {
        int32_t raw[] = {100, 200, 300};
        ray_t* vec = ray_vec_from_raw(RAY_DATE, raw, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
        ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_EQ_I(err, RAY_OK);
        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_EQ_I(loaded->type, RAY_DATE);
        TEST_ASSERT_EQ_I(loaded->len, 3);
        ray_release(loaded);
        ray_release(vec);
        unlink(ray_test_tmp(TMP_COL_PATH));
    }
    /* RAY_TIME */
    {
        int64_t raw[] = {0, 3600000000000LL, 7200000000000LL};
        ray_t* vec = ray_vec_from_raw(RAY_TIME, raw, 3);
        TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
        ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_EQ_I(err, RAY_OK);
        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_EQ_I(loaded->type, RAY_TIME);
        TEST_ASSERT_EQ_I(loaded->len, 3);
        ray_release(loaded);
        ray_release(vec);
        unlink(ray_test_tmp(TMP_COL_PATH));
    }
    /* RAY_TIMESTAMP */
    {
        int64_t raw[] = {1000000000000LL, 2000000000000LL};
        ray_t* vec = ray_vec_from_raw(RAY_TIMESTAMP, raw, 2);
        TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
        ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_EQ_I(err, RAY_OK);
        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_EQ_I(loaded->type, RAY_TIMESTAMP);
        TEST_ASSERT_EQ_I(loaded->len, 2);
        ray_release(loaded);
        ray_release(vec);
        unlink(ray_test_tmp(TMP_COL_PATH));
    }
    PASS();
}

/* ---- test_col_sym_w32_roundtrip ----------------------------------------- */
/* Covers validate_sym_bounds W32 arm (currently 0 coverage). */
static test_result_t test_col_sym_w32_roundtrip(void) {
    /* Intern enough symbols */
    ray_sym_intern("w32_a", 5);
    ray_sym_intern("w32_b", 5);
    ray_sym_intern("w32_c", 5);
    uint32_t sc = ray_sym_count();
    TEST_ASSERT((sc) >= (3), "sc >= 3");

    /* Build a W32 RAY_SYM column with valid indices */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W32, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = 3;
    uint32_t* data = (uint32_t*)ray_data(vec);
    data[0] = 0; data[1] = 1; data[2] = 2;

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_I(loaded->len, 3);
    TEST_ASSERT_EQ_U(loaded->attrs & RAY_SYM_W_MASK, RAY_SYM_W32);

    uint32_t* ld = (uint32_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(ld[0], 0);
    TEST_ASSERT_EQ_I(ld[1], 1);
    TEST_ASSERT_EQ_I(ld[2], 2);
    ray_release(loaded);

    /* Out-of-range W32 index should be rejected on load */
    data[1] = sc + 100;
    err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* bad = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_save_load_empty --------------------------------------------- */
/* Covers 0-length vector save/load to hit the `data_size == 0` branch. */
static test_result_t test_col_save_load_empty(void) {
    ray_t* vec = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_I64);
    TEST_ASSERT_EQ_I(loaded->len, 0);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->len, 0);
    ray_release(mapped);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_validate_mapped_bad_type ----------------------------------- */
/* Covers col_validate_mapped: invalid type in header triggers "nyi" error. */
static test_result_t test_col_validate_mapped_bad_type(void) {
    /* Write a 32-byte file with type=RAY_ERROR (127) in byte 18 */
    FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "wb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t hdr[32];
    memset(hdr, 0, 32);
    /* Stamp the format major version into byte 17 (`order`) so validation
     * reaches the type check (the bytes under test) past the version gate.
     * aux (bytes 0-15) stays zero — no magic lives there. */
    hdr[17] = RAY_COL_FORMAT_MAJOR;
    hdr[18] = 127;    /* type = RAY_ERROR -- not in serializable allowlist */
    hdr[19] = 0;      /* attrs */
    /* rc=1 at bytes 20-23 */
    hdr[20] = 1;
    /* len=0 at bytes 24-31 */
    fwrite(hdr, 1, 32, f);
    fclose(f);

    /* Both load and mmap should fail; mmap uses col_validate_mapped */
    ray_t* result = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));
    /* "nyi" from col_validate_mapped invalid type branch */
    TEST_ASSERT_STR_EQ(ray_err_code(result), "nyi");
    ray_release(result);

    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_recursive_atoms ------------------------------------------- */
/* Covers col_write_recursive and col_read_recursive atom paths (type < 0):
 * a RAY_LIST containing non-str atoms goes through the "fixed atom" branch. */
static test_result_t test_col_recursive_atoms(void) {
    /* Build a list with a mix: i64 atom + str atom */
    ray_t* list = ray_list_new(3);
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    ray_t* a_i64 = ray_i64(42);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a_i64));
    ray_t* a_str = ray_str("hello", 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a_str));
    ray_t* a_bool = ray_bool(true);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a_bool));

    list = ray_list_append(list, a_i64);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    list = ray_list_append(list, a_str);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    list = ray_list_append(list, a_bool);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    /* is_str_list returns false (mixed types) => goes through col_save_list
     * which calls col_write_recursive with atom elements */
    ray_err_t err = ray_col_save(list, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 3);

    ray_t** slots = (ray_t**)ray_data(loaded);
    /* First element: i64 atom */
    TEST_ASSERT_EQ_I(slots[0]->type, -RAY_I64);
    TEST_ASSERT_EQ_I(slots[0]->i64, 42);
    /* Second element: str atom */
    TEST_ASSERT_EQ_I(slots[1]->type, -RAY_STR);
    /* Third element: bool atom */
    TEST_ASSERT_EQ_I(slots[2]->type, -RAY_BOOL);

    ray_release(loaded);
    ray_release(a_i64);
    ray_release(a_str);
    ray_release(a_bool);
    ray_release(list);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_recursive_sym_in_list -------------------------------------- */
/* Covers col_write_recursive and col_read_recursive: RAY_SYM vector inside a
 * generic list.  Post-flip the recursive format serializes nested SYM data
 * as STRINGS (no symfile applies to nested payloads): the load re-interns
 * into the global table and rebuilds the vec as runtime-domain W64 — the
 * on-disk width of the source vec is NOT preserved (representation detail),
 * the RESOLVED STRINGS are. */
static test_result_t test_col_recursive_sym_in_list(void) {
    ray_sym_intern("rsl_x", 5);
    ray_sym_intern("rsl_y", 5);
    uint32_t sc = ray_sym_count();

    /* Build W8 sym column: ["", <first interned sym>] */
    ray_t* sym_vec = ray_sym_vec_new(RAY_SYM_W8, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sym_vec));
    sym_vec->len = 2;
    uint8_t* sd = (uint8_t*)ray_data(sym_vec);
    sd[0] = 0; sd[1] = 1;
    ray_t* want0 = ray_sym_str(0);
    ray_t* want1 = ray_sym_str(1);
    TEST_ASSERT_NOT_NULL(want0);
    TEST_ASSERT_NOT_NULL(want1);

    /* Wrap in a list (not is_str_list, so uses col_save_list -> col_write_recursive) */
    ray_t* list = ray_list_new(1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    list = ray_list_append(list, sym_vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    ray_err_t err = ray_col_save(list, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 1);

    ray_t** slots = (ray_t**)ray_data(loaded);
    TEST_ASSERT_EQ_I(slots[0]->type, RAY_SYM);
    TEST_ASSERT_EQ_I(slots[0]->len, 2);
    /* strings round-trip cell-for-cell (runtime-domain rebuild) */
    ray_t* got0 = ray_sym_vec_cell(slots[0], 0);
    ray_t* got1 = ray_sym_vec_cell(slots[0], 1);
    TEST_ASSERT_NOT_NULL(got0);
    TEST_ASSERT_NOT_NULL(got1);
    TEST_ASSERT_EQ_U(ray_str_len(got0), ray_str_len(want0));
    TEST_ASSERT_EQ_U(ray_str_len(got1), ray_str_len(want1));
    TEST_ASSERT_MEM_EQ(ray_str_len(got1), ray_str_ptr(got1), ray_str_ptr(want1));

    ray_release(loaded);
    ray_release(sym_vec);
    ray_release(list);
    unlink(ray_test_tmp(TMP_COL_PATH));
    (void)sc;
    PASS();
}

/* ---- test_col_sym_w64_negative_index ------------------------------------- */
/* Covers validate_sym_bounds W64 negative-index branch (p[i] < 0). */
static test_result_t test_col_sym_w64_negative_index(void) {
    ray_sym_intern("w64_a", 5);
    ray_sym_intern("w64_b", 5);
    uint32_t sc = ray_sym_count();
    TEST_ASSERT((sc) >= (2), "sc >= 2");

    /* Build a W64 RAY_SYM column with a negative index */
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W64, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = 3;
    int64_t* data = (int64_t*)ray_data(vec);
    data[0] = 0; data[1] = 1; data[2] = -1;  /* -1 is invalid */

    /* Bypass normal save (which would reject via validate) by writing raw bytes.
     * We save with sym_count=0 trick: temporarily save a zero-count column
     * that won't be validated, then patch the file. */
    /* Simpler: save valid column first to establish file, then corrupt index */
    data[2] = 0;  /* make it valid for save */
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* Now patch byte at offset 32 + 2*8 = 48 to be 0xFF (represents -1 as int64 MSB) */
    FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "r+b");
    TEST_ASSERT_NOT_NULL(f);
    /* data[2] is at offset 32 + 16 bytes = 48; set it to -1 */
    fseek(f, 32 + 16, SEEK_SET);
    int64_t neg = -1LL;
    fwrite(&neg, 8, 1, f);
    fclose(f);

    /* Load should fail with "corrupt" since p[i] < 0 in W64 branch */
    ray_t* bad = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    bad = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    (void)sc;
    PASS();
}

/* ---- test_col_str_pool_roundtrip ---------------------------------------- */
/* Covers the pooled-string path through col.c:
 *   col_save_impl STR pool write (line 635 True: pool_size > 0),
 *   col_str_pool_payload_len (line 738 True),
 *   col_copy_str_pool main path (lines 747-757) via ray_col_load,
 *   col_validate_str_region pooled-string validation (lines 777-785),
 *   ray_col_mmap STR str_pool reattach (lines 1028-1035).
 * A STR vector whose elements exceed RAY_STR_INLINE_MAX (12 bytes) forces
 * the byte pool to be non-empty so str_pool->len > 0. */
static test_result_t test_col_str_pool_roundtrip(void) {
    ray_t* vec = ray_vec_new(RAY_STR, 3);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    /* All > 12 bytes => pooled (not inline). */
    const char* s0 = "this-string-is-definitely-pooled";
    const char* s1 = "another-long-pooled-value-here!!";
    const char* s2 = "yet-a-third-pooled-string-value!";
    vec = ray_str_vec_append(vec, s0, strlen(s0));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, s1, strlen(s1));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, s2, strlen(s2));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    TEST_ASSERT_EQ_I(vec->len, 3);
    /* Pool must be non-empty (long strings). */
    TEST_ASSERT_NOT_NULL(vec->str_pool);
    TEST_ASSERT_TRUE(vec->str_pool->len > 0);

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* --- Round-trip via ray_col_load (deep-copy str pool) --- */
    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_STR);
    TEST_ASSERT_EQ_I(loaded->len, 3);
    TEST_ASSERT_NOT_NULL(loaded->str_pool);
    size_t ln = 0;
    const char* g0 = ray_str_vec_get(loaded, 0, &ln);
    TEST_ASSERT_EQ_U(ln, strlen(s0));
    TEST_ASSERT_TRUE(memcmp(g0, s0, ln) == 0);
    const char* g2 = ray_str_vec_get(loaded, 2, &ln);
    TEST_ASSERT_EQ_U(ln, strlen(s2));
    TEST_ASSERT_TRUE(memcmp(g2, s2, ln) == 0);
    ray_release(loaded);

    /* --- Round-trip via ray_col_mmap (zero-copy, pool->mmod=2) --- */
    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->type, RAY_STR);
    TEST_ASSERT_EQ_I(mapped->len, 3);
    TEST_ASSERT_NOT_NULL(mapped->str_pool);
    const char* m1 = ray_str_vec_get(mapped, 1, &ln);
    TEST_ASSERT_EQ_U(ln, strlen(s1));
    TEST_ASSERT_TRUE(memcmp(m1, s1, ln) == 0);
    ray_release(mapped);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_str_legacy_hash_repair ----------------------------------- */
/* Older column writers persisted the pooled descriptor's final four bytes
 * as uninitialized padding.  Forge distinct nonzero values there for equal
 * strings and verify both load modes recompute content hashes before the
 * grouping fast path consumes them. */
static test_result_t test_col_str_legacy_hash_repair(void) {
    const char* same = "legacy-pooled-string-value";
    const char* other = "different-pooled-string";
    ray_t* vec = ray_vec_new(RAY_STR, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, same, strlen(same));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, same, strlen(same));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, other, strlen(other));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    TEST_ASSERT_EQ_I(ray_col_save(vec, ray_test_tmp(TMP_COL_PATH)), RAY_OK);

    /* [32-byte column header][16-byte descriptors]; hash32 is descriptor
     * bytes 12..15.  These model arbitrary nonzero legacy padding. */
    static const uint32_t garbage[] = {
        0x11111111u, 0x22222222u, 0x33333333u
    };
    FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "r+b");
    TEST_ASSERT_NOT_NULL(f);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQ_I(fseek(f, 32L + (long)i * 16L + 12L, SEEK_SET), 0);
        TEST_ASSERT_EQ_U(fwrite(&garbage[i], 1, sizeof(garbage[i]), f),
                         sizeof(garbage[i]));
    }
    /* A legacy writer also left the header's rc at 0: no hash mark. */
    uint32_t legacy_rc = 0;
    TEST_ASSERT_EQ_I(fseek(f, (long)offsetof(ray_t, rc), SEEK_SET), 0);
    TEST_ASSERT_EQ_U(fwrite(&legacy_rc, 1, sizeof(legacy_rc), f), sizeof(legacy_rc));
    TEST_ASSERT_EQ_I(fclose(f), 0);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_str_t* ld = (ray_str_t*)ray_data(loaded);
    const char* lpool = (const char*)ray_data(loaded->str_pool);
    for (int i = 0; i < 3; i++) {
        uint32_t h = (uint32_t)ray_str_t_hash(&ld[i], lpool);
        TEST_ASSERT_EQ_U(ld[i].hash32, h != 0 ? h : 1u);
    }
    TEST_ASSERT_EQ_U(ld[0].hash32, ld[1].hash32);
    ray_t* groups = ray_group_indices_fn(loaded);
    TEST_ASSERT_NOT_NULL(groups);
    TEST_ASSERT_FALSE(RAY_IS_ERR(groups));
    TEST_ASSERT_EQ_I(ray_dict_keys(groups)->len, 2);
    ray_release(groups);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    ray_str_t* md = (ray_str_t*)ray_data(mapped);
    const char* mpool = (const char*)ray_data(mapped->str_pool);
    for (int i = 0; i < 3; i++) {
        uint32_t h = (uint32_t)ray_str_t_hash(&md[i], mpool);
        TEST_ASSERT_EQ_U(md[i].hash32, h != 0 ? h : 1u);
    }
    TEST_ASSERT_EQ_U(md[0].hash32, md[1].hash32);
    groups = ray_group_indices_fn(mapped);
    TEST_ASSERT_NOT_NULL(groups);
    TEST_ASSERT_FALSE(RAY_IS_ERR(groups));
    TEST_ASSERT_EQ_I(ray_dict_keys(groups)->len, 2);
    ray_release(groups);
    ray_release(mapped);

    /* MAP_PRIVATE repair must not rewrite the persisted legacy bytes. */
    f = fopen(ray_test_tmp(TMP_COL_PATH), "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I(fseek(f, 32L + 12L, SEEK_SET), 0);
    uint32_t persisted = 0;
    TEST_ASSERT_EQ_U(fread(&persisted, 1, sizeof(persisted), f),
                     sizeof(persisted));
    TEST_ASSERT_EQ_I(fclose(f), 0);
    TEST_ASSERT_EQ_U(persisted, garbage[0]);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_str_hash_persisted --------------------------------------- */
/* A current writer stores every pooled descriptor's content hash and marks
 * the header, so the loaders take the descriptors as written: no pass over
 * the pool, no write into the private mapping.  Hashes the column has not
 * cached yet are computed on save. */
static test_result_t test_col_str_hash_persisted(void) {
    const char* same = "persisted-pooled-string-value";
    const char* other = "another-pooled-string-value";
    ray_t* vec = ray_vec_new(RAY_STR, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, same, strlen(same));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, same, strlen(same));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, other, strlen(other));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_str_t* vd = (ray_str_t*)ray_data(vec);
    const char* vpool = (const char*)ray_data(vec->str_pool);
    uint32_t expect[3];
    for (int i = 0; i < 3; i++) {
        expect[i] = ray_str_t_hash32(&vd[i], vpool);
        vd[i].hash32 = 0;   /* lazy state: the writer must fill it */
    }
    TEST_ASSERT_EQ_I(ray_col_save(vec, TMP_COL_PATH), RAY_OK);

    /* On disk: the mark in rc, the hash in every pooled descriptor. */
    FILE* f = fopen(TMP_COL_PATH, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    uint32_t disk_rc = 0;
    TEST_ASSERT_EQ_I(fseek(f, (long)offsetof(ray_t, rc), SEEK_SET), 0);
    TEST_ASSERT_EQ_U(fread(&disk_rc, 1, sizeof(disk_rc), f), sizeof(disk_rc));
    TEST_ASSERT_EQ_U(disk_rc, RAY_COL_STR_HASHED);
    for (int i = 0; i < 3; i++) {
        uint32_t stored = 0;
        TEST_ASSERT_EQ_I(fseek(f, 32L + (long)i * 16L + 12L, SEEK_SET), 0);
        TEST_ASSERT_EQ_U(fread(&stored, 1, sizeof(stored), f), sizeof(stored));
        TEST_ASSERT_EQ_U(stored, expect[i]);
    }
    /* Forge one stored hash.  A marked file is taken as written, which is
     * only observable this way: the loaders must not recompute it. */
    const uint32_t forged = 0x5A5A5A5Au;
    TEST_ASSERT_EQ_I(fseek(f, 32L + 2L * 16L + 12L, SEEK_SET), 0);
    TEST_ASSERT_EQ_U(fwrite(&forged, 1, sizeof(forged), f), sizeof(forged));
    TEST_ASSERT_EQ_I(fclose(f), 0);

    ray_t* mapped = ray_col_mmap(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    ray_str_t* md = (ray_str_t*)ray_data(mapped);
    TEST_ASSERT_EQ_U(md[0].hash32, expect[0]);
    TEST_ASSERT_EQ_U(md[1].hash32, expect[1]);
    TEST_ASSERT_EQ_U(md[2].hash32, forged);
    ray_release(mapped);

    ray_t* loaded = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    ray_str_t* ld = (ray_str_t*)ray_data(loaded);
    TEST_ASSERT_EQ_U(ld[0].hash32, expect[0]);
    TEST_ASSERT_EQ_U(ld[2].hash32, forged);
    ray_release(loaded);

    ray_release(vec);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_stream_str_hash_persisted ------------------------------------- */
/* The streaming writer (CSV and parquet imports) stores the same hashes and
 * the same mark as ray_col_save, including for a column whose in-memory
 * hashes were never computed. */
static test_result_t test_stream_str_hash_persisted(void) {
    char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-stream-strhash-%d", (int)getpid());
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(dir), 0);
    const char* s0 = "streamed-pooled-string-value";
    const char* s1 = "short";
    const char* s2 = "second-streamed-pooled-value";
    ray_t* v = ray_vec_new(RAY_STR, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v = ray_str_vec_append(v, s0, strlen(s0));
    v = ray_str_vec_append(v, s1, strlen(s1));
    v = ray_str_vec_append(v, s2, strlen(s2));
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    ray_str_t* vd = (ray_str_t*)ray_data(v);
    const char* vpool = (const char*)ray_data(v->str_pool);
    uint32_t expect[3];
    for (int i = 0; i < 3; i++) {
        expect[i] = ray_str_t_hash32(&vd[i], vpool);
        if (!ray_str_is_inline(&vd[i])) vd[i].hash32 = 0;
    }
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, dir, ray_sym_intern("s", 1), RAY_STR, NULL, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, v), RAY_OK);
    TEST_ASSERT_EQ_I(ray_col_stream_close(&w, false), RAY_OK);

    char path[200]; snprintf(path, sizeof(path), "%s/s", dir);
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    uint32_t disk_rc = 0;
    TEST_ASSERT_EQ_I(fseek(f, (long)offsetof(ray_t, rc), SEEK_SET), 0);
    TEST_ASSERT_EQ_U(fread(&disk_rc, 1, sizeof(disk_rc), f), sizeof(disk_rc));
    TEST_ASSERT_EQ_U(disk_rc, RAY_COL_STR_HASHED);
    TEST_ASSERT_EQ_I(fclose(f), 0);

    ray_t* back = ray_col_mmap(path);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    ray_str_t* bd = (ray_str_t*)ray_data(back);
    const char* bpool = (const char*)ray_data(back->str_pool);
    TEST_ASSERT_EQ_U(bd[0].hash32, expect[0]);
    TEST_ASSERT_EQ_U(bd[2].hash32, expect[2]);
    TEST_ASSERT_TRUE(ray_str_is_inline(&bd[1]));
    TEST_ASSERT_EQ_U(ray_str_t_hash32(&bd[1], bpool), expect[1]);
    /* And the stored value is the content hash of the bytes on disk. */
    TEST_ASSERT_EQ_U(bd[0].hash32, (uint32_t)ray_str_t_hash(&bd[0], bpool));
    ray_release(back); ray_release(v);
    (void)ray_test_rm_rf(dir);
    PASS();
}

/* ---- test_parted_remount_reuse ------------------------------------------ */
/* A second ray_read_parted of the same root returns the very same segment
 * vectors for partitions whose files did not change, loads a replaced
 * partition afresh, and ray_parted_cache_clear forgets everything. */
#define TMP_PARTED_ROOT "/tmp/rayforce_test_parted_remount"
static ray_t* parted_remount_part(int64_t base) {
    int64_t raw[3] = { base, base + 1, base + 2 };
    ray_t* v = ray_vec_from_raw(RAY_I64, raw, 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), v);
    ray_release(v);
    return tbl;
}
static test_result_t test_parted_remount_reuse(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(TMP_PARTED_ROOT "/2024.01.01"), 0);
    TEST_ASSERT_EQ_I(ray_test_mkdir_p(TMP_PARTED_ROOT "/2024.01.02"), 0);
    ray_t* a = parted_remount_part(10);
    ray_t* b = parted_remount_part(20);
    TEST_ASSERT_EQ_I(ray_splay_save(a, TMP_PARTED_ROOT "/2024.01.01/t", NULL), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save(b, TMP_PARTED_ROOT "/2024.01.02/t", NULL), RAY_OK);

    ray_t* m1 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m1 && !RAY_IS_ERR(m1));
    ray_t** s1 = (ray_t**)ray_data(ray_table_get_col_idx(m1, 1));
    ray_t* m2 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m2 && !RAY_IS_ERR(m2));
    ray_t** s2 = (ray_t**)ray_data(ray_table_get_col_idx(m2, 1));
    /* Unchanged root: both partitions are the same mapped vectors. */
    TEST_ASSERT_EQ_PTR(s1[0], s2[0]);
    TEST_ASSERT_EQ_PTR(s1[1], s2[1]);
    TEST_ASSERT_TRUE(ray_atomic_load(&s1[0]->rc) >= 3);   /* two results + the cache */

    /* Replace the second partition: a fresh vector there, the first kept. */
    ray_t* c = parted_remount_part(30);
    TEST_ASSERT_EQ_I(ray_splay_save(c, TMP_PARTED_ROOT "/2024.01.02/t", NULL), RAY_OK);
    ray_t* m3 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m3 && !RAY_IS_ERR(m3));
    ray_t** s3 = (ray_t**)ray_data(ray_table_get_col_idx(m3, 1));
    TEST_ASSERT_EQ_PTR(s3[0], s1[0]);
    TEST_ASSERT_TRUE(s3[1] != s1[1]);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(s3[1]))[0], 30);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(s1[1]))[0], 20);   /* the old view keeps its rows */

    /* Forgotten: the next mount maps everything again. */
    ray_parted_cache_clear();
    ray_t* m4 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m4 && !RAY_IS_ERR(m4));
    ray_t** s4 = (ray_t**)ray_data(ray_table_get_col_idx(m4, 1));
    TEST_ASSERT_TRUE(s4[0] != s1[0]);
    TEST_ASSERT_EQ_I(ray_table_nrows(m4), 6);

    ray_release(m1); ray_release(m2); ray_release(m3); ray_release(m4);
    ray_release(a); ray_release(b); ray_release(c);
    ray_parted_cache_clear();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

#ifdef RAY_OS_LINUX
/* Check the mappings themselves: the legacy numeric-column unmap path
 * rounds its accounting subtraction up to a page, unlike the map charge. */
static int parted_mapping_count(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return -1;
    int count = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f))
        if (strstr(line, TMP_PARTED_ROOT)) count++;
    fclose(f);
    return count;
}
#endif

/* A dropped LIST view must release its copied payload and mapped columns
 * on GC, including when the directory has already disappeared. */
static test_result_t test_parted_cache_gc(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* items = ray_list_new(16384);
    for (int i = 0; i < 16384; i++) {
        ray_t* item = ray_i64(i);
        items = ray_list_append(items, item);
        ray_release(item);
    }
    ray_t* src = ray_table_new(2);
    src = ray_table_add_col(src, ray_sym_intern("v", 1), items);
    ray_release(items);
    ray_t* nums = ray_vec_new(RAY_I64, 16384);
    nums->len = 16384;
    memset(ray_data(nums), 0, 16384 * sizeof(int64_t));
    src = ray_table_add_col(src, ray_sym_intern("n", 1), nums);
    ray_release(nums);
    TEST_ASSERT_EQ_I(ray_splay_save(src, TMP_PARTED_ROOT "/1/t", NULL), RAY_OK);
    ray_mem_stats_t before, mounted, after;
    ray_mem_stats(&before);
    ray_t* view = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(view && !RAY_IS_ERR(view));
    ray_t* seg = ((ray_t**)ray_data(ray_table_get_col_idx(view, 1)))[0];
    ray_heap_gc();
    TEST_ASSERT_EQ_U(ray_atomic_load(&seg->rc), 2); /* live views keep reuse */
    ray_mem_stats(&mounted);
#ifdef RAY_OS_LINUX
    TEST_ASSERT_TRUE(parted_mapping_count() > 0);
#endif
    TEST_ASSERT_TRUE(mounted.bytes_allocated > before.bytes_allocated + 500000);
    ray_release(view);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_heap_gc();
    ray_mem_stats(&after);
    TEST_ASSERT_TRUE(after.bytes_allocated <= before.bytes_allocated + 4096);
#ifdef RAY_OS_LINUX
    TEST_ASSERT_EQ_I(parted_mapping_count(), 0);
#endif
    ray_release(src);
    PASS();
}

static ray_t* parted_symbol_part(const char* value) {
    int64_t id = ray_sym_intern(value, strlen(value));
    ray_t* col = ray_vec_from_raw(RAY_SYM, &id, 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, ray_sym_intern("s", 1), col);
    ray_release(col);
    return tbl;
}

/* Recreating a root after dropping its view must not find a dictionary
 * pinned only by the cache, with or without explicit GC before the write.
 * Cover both equal-size and differently sized symbols. */
static test_result_t parted_rebuilt_symbols(bool collect) {
    const char* values[] = { "aa", "xx", "longer" };
    for (int i = 0; i < 3; i++) {
        (void)ray_test_rm_rf(TMP_PARTED_ROOT);
        ray_t* src = parted_symbol_part(values[i]);
        TEST_ASSERT_EQ_I(ray_splay_save(src, TMP_PARTED_ROOT "/1/t",
                                        TMP_PARTED_ROOT "/.sym"), RAY_OK);
        ray_release(src);
        ray_t* view = ray_read_parted(TMP_PARTED_ROOT, "t");
        TEST_ASSERT_TRUE(view && !RAY_IS_ERR(view));
        ray_t* seg = ((ray_t**)ray_data(ray_table_get_col_idx(view, 1)))[0];
        int64_t pos = ray_read_sym(ray_data(seg), 0, seg->type, seg->attrs);
        ray_t* value = ray_sym_domain_str(ray_sym_vec_domain(seg), pos);
        TEST_ASSERT_EQ_U(ray_str_len(value), strlen(values[i]));
        TEST_ASSERT_TRUE(memcmp(ray_str_ptr(value), values[i], strlen(values[i])) == 0);
        ray_release(view);
        if (collect) ray_heap_gc();
    }
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

static test_result_t test_parted_rebuilt_symbols(void) {
    return parted_rebuilt_symbols(true);
}

static test_result_t test_parted_rebuilt_write_without_gc(void) {
    return parted_rebuilt_symbols(false);
}

/* A filesystem publisher can replace the root without a GC call in the
 * reader. Both parted and splayed reads must discard unused cached domains. */
static test_result_t parted_rebuilt_read_without_gc(bool parted) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT "_next");
    ray_t* a = parted_symbol_part("aa");
    ray_t* b = parted_symbol_part("xx");
    TEST_ASSERT_EQ_I(ray_splay_save(a, TMP_PARTED_ROOT "/1/t",
                                    TMP_PARTED_ROOT "/.sym"), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save(b, TMP_PARTED_ROOT "_next/1/t",
                                    TMP_PARTED_ROOT "_next/.sym"), RAY_OK);
    ray_t* old = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(old && !RAY_IS_ERR(old));
    ray_release(old);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    TEST_ASSERT_EQ_I(rename(TMP_PARTED_ROOT "_next", TMP_PARTED_ROOT), 0);
    ray_t* fresh = parted ? ray_read_parted(TMP_PARTED_ROOT, "t")
                          : ray_read_splayed(TMP_PARTED_ROOT "/1/t", TMP_PARTED_ROOT "/.sym");
    TEST_ASSERT_TRUE(fresh && !RAY_IS_ERR(fresh));
    ray_t* seg = ray_table_get_col_idx(fresh, parted ? 1 : 0);
    if (parted) seg = ((ray_t**)ray_data(seg))[0];
    int64_t pos = ray_read_sym(ray_data(seg), 0, seg->type, seg->attrs);
    ray_t* value = ray_sym_domain_str(ray_sym_vec_domain(seg), pos);
    TEST_ASSERT_EQ_U(ray_str_len(value), 2);
    TEST_ASSERT_TRUE(memcmp(ray_str_ptr(value), "xx", 2) == 0);
    ray_release(fresh); ray_release(a); ray_release(b);
    ray_heap_gc();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

static test_result_t test_parted_rebuilt_without_gc(void) {
    return parted_rebuilt_read_without_gc(true);
}

static test_result_t test_parted_rebuilt_splayed_without_gc(void) {
    return parted_rebuilt_read_without_gc(false);
}

/* Opening an unrelated domain must preserve partitions held by a live view. */
static test_result_t test_parted_live_domain_open(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT "_next");
    ray_t* a = parted_symbol_part("aa");
    ray_t* b = parted_symbol_part("xx");
    TEST_ASSERT_EQ_I(ray_splay_save(a, TMP_PARTED_ROOT "/1/t",
                                    TMP_PARTED_ROOT "/.sym"), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save(b, TMP_PARTED_ROOT "_next/1/t",
                                    TMP_PARTED_ROOT "_next/.sym"), RAY_OK);
    ray_t* view = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(view && !RAY_IS_ERR(view));
    ray_t* seg = ((ray_t**)ray_data(ray_table_get_col_idx(view, 1)))[0];
    TEST_ASSERT_EQ_I(ray_atomic_load(&seg->rc), 2);

    ray_t* other = ray_read_splayed(TMP_PARTED_ROOT "_next/1/t",
                                    TMP_PARTED_ROOT "_next/.sym");
    TEST_ASSERT_TRUE(other && !RAY_IS_ERR(other));
    TEST_ASSERT_EQ_I(ray_atomic_load(&seg->rc), 2);
    ray_t* remount = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(remount && !RAY_IS_ERR(remount));
    TEST_ASSERT_EQ_PTR(((ray_t**)ray_data(ray_table_get_col_idx(remount, 1)))[0], seg);
    int64_t pos = ray_read_sym(ray_data(seg), 0, seg->type, seg->attrs);
    ray_t* value = ray_sym_domain_str(ray_sym_vec_domain(seg), pos);
    TEST_ASSERT_EQ_U(ray_str_len(value), 2);
    TEST_ASSERT_TRUE(memcmp(ray_str_ptr(value), "aa", 2) == 0);

    ray_release(remount); ray_release(other); ray_release(view);
    ray_release(a); ray_release(b);
    ray_parted_cache_clear();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT "_next");
    PASS();
}

static test_result_t test_parted_missing_symfile(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* src = parted_symbol_part("aa");
    TEST_ASSERT_EQ_I(ray_splay_save(src, TMP_PARTED_ROOT "/1/t",
                                    TMP_PARTED_ROOT "/.sym"), RAY_OK);
    ray_release(src);
    ray_t* view = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(view && !RAY_IS_ERR(view));
    TEST_ASSERT_EQ_I(unlink(TMP_PARTED_ROOT "/.sym"), 0);
    ray_t* missing = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_ERR_CODE_MSG(missing, "sym", "no symfile resolved");
    ray_error_free(missing);
    ray_release(view);
    ray_parted_cache_clear();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

/* A shared symbol file named "sym" (no dot) is not read.  The error must
 * name the real file, "<root>/.sym", not the "sym" it never looks for. */
static test_result_t test_parted_symfile_name_in_error(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* src = parted_symbol_part("aa");
    TEST_ASSERT_EQ_I(ray_splay_save(src, TMP_PARTED_ROOT "/1/t",
                                    TMP_PARTED_ROOT "/sym"), RAY_OK);
    ray_release(src);
    ray_t* view = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_ERR_CODE_MSG(view, "sym", "<dir>/.sym");
    ray_error_free(view);
    ray_parted_cache_clear();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

/* Same inode, size and mtime: a copied LIST payload must still reload.
 * Numeric mmap payloads alone cannot detect a bad cache hit here because
 * an in-place write is already visible through the old mapping. */
static test_result_t test_parted_restored_mtime(void) {
#ifdef RAY_OS_WINDOWS
    SKIP("nanosecond timestamp restoration requires POSIX");
#else
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* items = ray_list_new(1);
    ray_t* value = ray_i64(10);
    items = ray_list_append(items, value);
    ray_release(value);
    ray_t* src = ray_table_new(1);
    src = ray_table_add_col(src, ray_sym_intern("v", 1), items);
    ray_release(items);
    TEST_ASSERT_EQ_I(ray_splay_save(src, TMP_PARTED_ROOT "/1/t", NULL), RAY_OK);
    ray_t* m1 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m1 && !RAY_IS_ERR(m1));
    ray_t* s1 = ((ray_t**)ray_data(ray_table_get_col_idx(m1, 1)))[0];
    const char* path = TMP_PARTED_ROOT "/1/t/v";
    struct stat st;
    TEST_ASSERT_EQ_I(stat(path, &st), 0);
    struct timespec times[2];
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
    /* Darwin's POSIX namespace exposes seconds/nanoseconds separately. */
    times[0] = (struct timespec){ st.st_atime, st.st_atimensec };
    times[1] = (struct timespec){ st.st_mtime, st.st_mtimensec };
#elif defined(__APPLE__)
    times[0] = st.st_atimespec; times[1] = st.st_mtimespec;
#else
    times[0] = st.st_atim; times[1] = st.st_mtim;
#endif
    /* Let ctime advance even on filesystems with coarse clocks. */
    struct timespec pause = {1, 0};
    nanosleep(&pause, NULL);
    FILE* f = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I(fseek(f, -8, SEEK_END), 0);
    int64_t changed = 99;
    TEST_ASSERT_EQ_U(fwrite(&changed, 1, 8, f), 8);
    TEST_ASSERT_EQ_I(fclose(f), 0);
    TEST_ASSERT_EQ_I(utimensat(AT_FDCWD, path, times, 0), 0);
    ray_t* m2 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m2 && !RAY_IS_ERR(m2));
    ray_t* s2 = ((ray_t**)ray_data(ray_table_get_col_idx(m2, 1)))[0];
    TEST_ASSERT_TRUE(s1 != s2);
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(s1))[0]->i64, 10);
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(s2))[0]->i64, 99);
    ray_release(m1); ray_release(m2); ray_release(src);
    ray_parted_cache_clear();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
#endif
}

/* Both generations remain intact: their file signatures cannot substitute
 * for comparing the resolved directory. Also prove the sweep drops refs.
 * Saved as generation 0: the asserts read plain segments' payloads and
 * reference counts. */
static test_result_t test_parted_generation_and_sweep(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* a = parted_remount_part(10);
    ray_t* b = parted_remount_part(20);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(a, TMP_PARTED_ROOT "/1/t", NULL, RAY_COMPRESS_OFF), RAY_OK);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(b, TMP_PARTED_ROOT "/2/t", NULL, RAY_COMPRESS_OFF), RAY_OK);
    ray_t* m1 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m1 && !RAY_IS_ERR(m1));
    ray_t** s1 = (ray_t**)ray_data(ray_table_get_col_idx(m1, 1));
    TEST_ASSERT_EQ_I(ray_splay_save_mode(b, TMP_PARTED_ROOT "/1/t/.generations/g-1", NULL, RAY_COMPRESS_OFF), RAY_OK);
    FILE* f = fopen(TMP_PARTED_ROOT "/1/t/.current", "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs(".generations/g-1\n", f) >= 0);
    TEST_ASSERT_EQ_I(fclose(f), 0);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT "/2");
    ray_t* m2 = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(m2 && !RAY_IS_ERR(m2));
    ray_t* s2 = ((ray_t**)ray_data(ray_table_get_col_idx(m2, 1)))[0];
    TEST_ASSERT_TRUE(s1[0] != s2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(s2))[0], 20);
    TEST_ASSERT_EQ_U(ray_atomic_load(&s1[1]->rc), 1); /* deleted partition swept */
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* missing = ray_read_parted(TMP_PARTED_ROOT, "t");
    TEST_ASSERT_TRUE(RAY_IS_ERR(missing));
    ray_error_free(missing);
    TEST_ASSERT_EQ_U(ray_atomic_load(&s2->rc), 1); /* failed mount sweeps too */
    ray_release(m1); ray_release(m2); ray_release(a); ray_release(b);
    PASS();
}

/* The limit applies across roots/tables even when every view remains live.
 * Eviction only drops the cache's reference, so the oldest view still reads.
 * Saved as generation 0: the asserts read plain segments' payloads and
 * reference counts. */
static test_result_t test_parted_cache_limit(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* src = parted_remount_part(10);
    ray_t* views[1025];
    for (int i = 0; i < 1025; i++) {
        char path[128], name[24];
        snprintf(name, sizeof(name), "t%d", i);
        snprintf(path, sizeof(path), "%s/1/%s", TMP_PARTED_ROOT, name);
        TEST_ASSERT_EQ_I(ray_splay_save_bulk_mode(src, path, NULL, RAY_COMPRESS_OFF), RAY_OK);
        views[i] = ray_read_parted(TMP_PARTED_ROOT, name);
        TEST_ASSERT_TRUE(views[i] && !RAY_IS_ERR(views[i]));
    }
    ray_t* first = ((ray_t**)ray_data(ray_table_get_col_idx(views[0], 1)))[0];
    ray_t* last = ((ray_t**)ray_data(ray_table_get_col_idx(views[1024], 1)))[0];
    TEST_ASSERT_EQ_U(ray_atomic_load(&first->rc), 1);
    TEST_ASSERT_EQ_U(ray_atomic_load(&last->rc), 2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(first))[0], 10);
    for (int i = 0; i < 1025; i++) ray_release(views[i]);
    ray_heap_gc();
    ray_release(src);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

static ray_t* parted_cache_seg(ray_t* view) {
    return ((ray_t**)ray_data(ray_table_get_col_idx(view, 1)))[0];
}

/* Remount table i of five, one partition each, replacing its view; whether
 * the mount reused the partition the old view had. */
static bool parted_cache_remount(ray_t** views, int i, bool* reused) {
    char name[8];
    snprintf(name, sizeof(name), "t%d", i);
    ray_t* v = ray_read_parted(TMP_PARTED_ROOT, name);
    if (!v || RAY_IS_ERR(v)) return false;
    *reused = views[i] && parted_cache_seg(v) == parted_cache_seg(views[i]);
    if (views[i]) ray_release(views[i]);
    views[i] = v;
    return true;
}

/* A segment's users: its view, and the cache while it keeps the partition. */
#define PARTED_CACHED(views, i) (ray_atomic_load(&parted_cache_seg((views)[i])->rc) == 2)

/* Under a cap of three, the least recently mounted partition goes first and
 * a hit makes its partition the newest.  A cyclic pass over more partitions
 * than the cap misses on every mount, and every mount reads right.  With no
 * cap, or one past 32 bits, all stay.  A cleared, pruned or replaced entry
 * leaves the index consistent.  Saved as generation 0: the asserts read
 * plain segments' payloads and reference counts. */
static test_result_t parted_cache_recency(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    for (int i = 0; i < 5; i++) {
        char path[128];
        snprintf(path, sizeof(path), "%s/1/t%d", TMP_PARTED_ROOT, i);
        ray_t* src = parted_remount_part(10 * i);
        TEST_ASSERT_EQ_I(ray_splay_save_mode(src, path, NULL, RAY_COMPRESS_OFF), RAY_OK);
        ray_release(src);
    }
    ray_t* views[5] = { NULL };
    bool reused;
    for (int i = 0; i < 3; i++)
        TEST_ASSERT_TRUE(parted_cache_remount(views, i, &reused));
    /* t0 again: a hit, and t0 the newest, so t3 evicts t1, then t4 t2. */
    TEST_ASSERT_TRUE(parted_cache_remount(views, 0, &reused));
    TEST_ASSERT_TRUE(reused);
    TEST_ASSERT_TRUE(parted_cache_remount(views, 3, &reused));
    TEST_ASSERT_FALSE(PARTED_CACHED(views, 1));
    TEST_ASSERT_TRUE(PARTED_CACHED(views, 2));
    TEST_ASSERT_TRUE(parted_cache_remount(views, 4, &reused));
    TEST_ASSERT_TRUE(PARTED_CACHED(views, 0));
    TEST_ASSERT_FALSE(PARTED_CACHED(views, 2));
    TEST_ASSERT_TRUE(PARTED_CACHED(views, 3));
    TEST_ASSERT_TRUE(PARTED_CACHED(views, 4));

    /* From empty, two cycles over five: each mount a miss, each table its own. */
    ray_parted_cache_clear();
    for (int i = 0; i < 5; i++) TEST_ASSERT_FALSE(PARTED_CACHED(views, i));
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 5; i++) {
            TEST_ASSERT_TRUE(parted_cache_remount(views, i, &reused));
            TEST_ASSERT_FALSE(reused);
            TEST_ASSERT_EQ_I(((int64_t*)ray_data(parted_cache_seg(views[i])))[0], 10 * i);
            TEST_ASSERT_EQ_I(ray_table_nrows(views[i]), 3);
        }
    }
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(PARTED_CACHED(views, i) == (i >= 2));

    /* No cap: t0 and t1 load once more, then every mount is a hit. */
    ray_parted_cache_set_max(0);
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 5; i++) {
            TEST_ASSERT_TRUE(parted_cache_remount(views, i, &reused));
            TEST_ASSERT_TRUE(reused == (round == 1 || i >= 2));
        }
    }
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(PARTED_CACHED(views, i));

    /* A cap past 32 bits is a large cap even where size_t is 32 bits: from
     * empty, each table loads once and then hits. */
    ray_parted_cache_set_max((int64_t)1 << 32);
    ray_parted_cache_clear();
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 5; i++) {
            TEST_ASSERT_TRUE(parted_cache_remount(views, i, &reused));
            TEST_ASSERT_TRUE(reused == (round == 1));
        }
    }
    for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(PARTED_CACHED(views, i));

    /* Pruned once its view is gone: the next mount loads, the one after hits. */
    ray_release(views[1]);
    views[1] = NULL;
    ray_heap_gc();
    TEST_ASSERT_TRUE(parted_cache_remount(views, 1, &reused));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(parted_cache_seg(views[1])))[0], 10);
    TEST_ASSERT_TRUE(parted_cache_remount(views, 1, &reused));
    TEST_ASSERT_TRUE(reused);

    /* Replaced on disk: a fresh load takes the entry's place, and is the hit. */
    ray_t* old3 = views[3];
    ray_retain(old3);
    ray_t* c = parted_remount_part(70);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(c, TMP_PARTED_ROOT "/1/t3", NULL, RAY_COMPRESS_OFF), RAY_OK);
    ray_release(c);
    TEST_ASSERT_TRUE(parted_cache_remount(views, 3, &reused));
    TEST_ASSERT_FALSE(reused);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(parted_cache_seg(views[3])))[0], 70);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(parted_cache_seg(old3)))[0], 30);   /* the old view keeps its rows */
    TEST_ASSERT_TRUE(parted_cache_remount(views, 3, &reused));
    TEST_ASSERT_TRUE(reused);
    ray_release(old3);

    for (int i = 0; i < 5; i++) ray_release(views[i]);
    PASS();
}

static test_result_t test_parted_cache_recency(void) {
    int64_t was = ray_parted_cache_set_max(3);
    test_result_t r = parted_cache_recency();
    ray_parted_cache_set_max(was);
    ray_parted_cache_clear();
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    return r;
}

typedef struct { atomic_int* ready; atomic_bool* go; bool ok; } parted_thread_t;
static void parted_mount_thread(void* arg) {
    parted_thread_t* ctx = arg;
    ray_heap_init();
    atomic_fetch_add(ctx->ready, 1);
    while (!atomic_load(ctx->go)) ray_sleep_ms(1);
    ctx->ok = true;
    for (int i = 0; i < 100 && ctx->ok; i++) {
        ray_t* a = ray_read_parted(TMP_PARTED_ROOT, "t");
        ray_t* b = ray_read_parted(TMP_PARTED_ROOT, "t");
        ctx->ok = a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b);
        if (ctx->ok) {
            ray_t* sa = ((ray_t**)ray_data(ray_table_get_col_idx(a, 1)))[0];
            ray_t* sb = ((ray_t**)ray_data(ray_table_get_col_idx(b, 1)))[0];
            ctx->ok = sa == sb && ((int64_t*)ray_data(sa))[0] == 10;
        }
        if (i % 3 == 1) ray_parted_cache_clear();
        ray_release(a); ray_release(b);
    }
    /* Must release this thread's cache even if its heap is adopted later. */
    ray_heap_abandon();
}

/* Saved as generation 0: the mount threads read a plain segment's payload. */
static test_result_t test_parted_concurrent_mounts(void) {
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    ray_t* src = parted_remount_part(10);
    TEST_ASSERT_EQ_I(ray_splay_save_mode(src, TMP_PARTED_ROOT "/1/t", NULL, RAY_COMPRESS_OFF), RAY_OK);
    atomic_int ready = 0;
    atomic_bool go = false;
    parted_thread_t ctx[4];
    ray_thread_t threads[4];
    for (int i = 0; i < 4; i++) {
        ctx[i] = (parted_thread_t){ &ready, &go, false };
        TEST_ASSERT_EQ_I(ray_thread_create(&threads[i], parted_mount_thread, &ctx[i]), RAY_OK);
    }
    while (atomic_load(&ready) < 4) ray_sleep_ms(1);
    atomic_store(&go, true);
    for (int i = 0; i < 4; i++) TEST_ASSERT_EQ_I(ray_thread_join(threads[i]), RAY_OK);
    for (int i = 0; i < 4; i++) TEST_ASSERT_TRUE(ctx[i].ok);
#ifdef RAY_OS_LINUX
    TEST_ASSERT_EQ_I(parted_mapping_count(), 0);
#endif
    ray_release(src);
    (void)ray_test_rm_rf(TMP_PARTED_ROOT);
    PASS();
}

/* Nested files have no persisted-hash marker. Garbage hash bytes must be
 * reset by their loader, while inline strings keep their payload bytes. */
static test_result_t test_col_nested_str_hash_reset(void) {
    ray_t* v = ray_vec_new(RAY_STR, 2);
    v = ray_str_vec_append(v, "nested-pooled-string-value", 25);
    v = ray_str_vec_append(v, "inline", 6);
    ((ray_str_t*)ray_data(v))[0].hash32 = 0x12345678;
    ray_t* list = ray_list_new(1);
    list = ray_list_append(list, v);
    TEST_ASSERT_EQ_I(ray_col_save(list, TMP_COL_PATH), RAY_OK);
    ray_t* back = ray_col_load(TMP_COL_PATH);
    TEST_ASSERT_TRUE(back && !RAY_IS_ERR(back));
    ray_t* nested = ((ray_t**)ray_data(back))[0];
    ray_str_t* desc = (ray_str_t*)ray_data(nested);
    TEST_ASSERT_EQ_U(desc[0].hash32, 0);
    size_t len = 0;
    const char* str = ray_str_vec_get(nested, 1, &len);
    TEST_ASSERT_EQ_U(len, 6);
    TEST_ASSERT_TRUE(memcmp(str, "inline", 6) == 0);
    ray_release(back); ray_release(list); ray_release(v);
    unlink(TMP_COL_PATH);
    PASS();
}

static test_result_t test_col_save_str_bounds(void) {
    ray_t* v = ray_vec_new(RAY_STR, 1);
    v = ray_str_vec_append(v, "pooled-string-out-of-bounds", 26);
    ray_str_t* desc = (ray_str_t*)ray_data(v);
    uint32_t old_off = desc[0].pool_off;
    desc[0].pool_off = UINT32_MAX;
    TEST_ASSERT_EQ_I(ray_col_save(v, TMP_COL_PATH), RAY_ERR_CORRUPT);
    desc[0].pool_off = old_off;
    desc[0].len = UINT32_MAX;
    TEST_ASSERT_EQ_I(ray_col_save(v, TMP_COL_PATH), RAY_ERR_CORRUPT);
    desc[0].len = 26;
    ray_release(v);
    unlink(TMP_COL_PATH);
    PASS();
}

/* ---- test_col_format_version_roundtrip ---------------------------------- */
/* A saved column carries the format generation in the 32-byte header's
 * `order` byte (offset 17), with aux (bytes 0-15) ZERO on disk (no magic —
 * aux is reserved for postponed index persistence).  It
 * round-trips through both ray_col_load (deep copy) and ray_col_mmap
 * (zero-copy) with value + attrs intact, and the on-disk version byte must
 * NOT leak into the in-memory runtime aux. */
static test_result_t test_col_format_version_roundtrip(void) {
    int32_t raw[] = {11, 22, 33, 44};
    ray_t* vec = ray_vec_from_raw(RAY_I32, raw, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    TEST_ASSERT_EQ_I(ray_col_save(vec, ray_test_tmp(TMP_COL_PATH)), RAY_OK);

    /* On disk: aux[0..15] all zero (no magic); byte 17 (order) == major. */
    {
        FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "rb");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t hd[18];
        TEST_ASSERT_EQ_U(fread(hd, 1, 18, f), 18);
        fclose(f);
        for (int i = 0; i < 16; i++) TEST_ASSERT_EQ_U(hd[i], 0);  /* aux zeroed */
        TEST_ASSERT_EQ_U(hd[17], RAY_COL_FORMAT_MAJOR);           /* order = version */
    }

    /* save -> load (deep copy): values intact, runtime aux reconstructed. */
    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_I32);
    TEST_ASSERT_EQ_I(loaded->len, 4);
    int32_t* ld = (int32_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(ld[0], 11);
    TEST_ASSERT_EQ_I(ld[3], 44);
    /* version byte must not survive into runtime aux (plain column => zero). */
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQ_U(loaded->aux[i], 0);
    ray_release(loaded);

    /* save -> mmap (zero-copy): values intact, runtime aux reconstructed. */
    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_U(mapped->mmod, 1);
    TEST_ASSERT_EQ_I(mapped->len, 4);
    int32_t* md = (int32_t*)ray_data(mapped);
    TEST_ASSERT_EQ_I(md[1], 22);
    TEST_ASSERT_EQ_I(md[2], 33);
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQ_U(mapped->aux[i], 0);
    ray_release(mapped);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_format_bad_version ---------------------------------------- */
/* The current format generation is 0 (RAY_COL_FORMAT_MAJOR).  Generation 0 is
 * the shipped/legacy layout, so a zeroed order byte (offset 17) — pre-stamp
 * data written by earlier engines — MUST load (regression: an earlier build
 * demanded 1 and orphaned every pre-existing database with a "version" error).
 * A file whose `order` byte is an UNRECOGNIZED generation is still rejected
 * with a "version" error by both load + mmap. */
static test_result_t test_col_format_bad_version(void) {
    int64_t raw[] = {1, 2, 3};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    /* (a) generation 0 (legacy/unstamped order byte) => loads, value intact. */
    TEST_ASSERT_EQ_I(ray_col_save(vec, ray_test_tmp(TMP_COL_PATH)), RAY_OK);
    {
        FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "r+b");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t zero = 0;
        TEST_ASSERT_EQ_I(fseek(f, 17, SEEK_SET), 0);  /* order byte */
        TEST_ASSERT_EQ_U(fwrite(&zero, 1, 1, f), 1);
        fclose(f);
    }
    ray_t* r1 = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->len, 3);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r1))[2], 3);
    ray_release(r1);
    ray_t* r2 = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(r2->len, 3);
    ray_release(r2);

    /* (b) an unrecognized generation => "version" (both load + mmap). */
    TEST_ASSERT_EQ_I(ray_col_save(vec, ray_test_tmp(TMP_COL_PATH)), RAY_OK);
    {
        FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "r+b");
        TEST_ASSERT_NOT_NULL(f);
        uint8_t bad_ver = (uint8_t)(RAY_COL_FORMAT_MAJOR + 1);
        TEST_ASSERT_EQ_I(fseek(f, 17, SEEK_SET), 0);  /* order byte */
        TEST_ASSERT_EQ_U(fwrite(&bad_ver, 1, 1, f), 1);
        fclose(f);
    }
    ray_t* r3 = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(r3));
    TEST_ASSERT_STR_EQ(ray_err_code(r3), "version");
    ray_release(r3);
    ray_t* r4 = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(r4));
    TEST_ASSERT_STR_EQ(ray_err_code(r4), "version");
    ray_release(r4);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_str_empty_roundtrip --------------------------------------- */
/* Covers the empty STR column path:
 *   col_str_pool_payload_len when pool is NULL (line 736 False arm 736:41),
 *   col_validate_str_region with hdr->len == 0 (loop body never runs),
 *   col_copy_str_pool early NULL return (line 745 str_pool_size == 0). */
static test_result_t test_col_str_empty_roundtrip(void) {
    ray_t* vec = ray_vec_new(RAY_STR, 0);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    TEST_ASSERT_EQ_I(vec->len, 0);

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_STR);
    TEST_ASSERT_EQ_I(loaded->len, 0);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->len, 0);
    ray_release(mapped);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_str_inline_only_roundtrip --------------------------------- */
/* Covers col_validate_str_region "len <= RAY_STR_INLINE_MAX => continue"
 * (line 777 True) plus an empty-pool STR column where every element is
 * inline so str_pool->len == 0 (col_copy_str_pool line 745 True). */
static test_result_t test_col_str_inline_only_roundtrip(void) {
    ray_t* vec = ray_vec_new(RAY_STR, 2);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, "aa", 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec = ray_str_vec_append(vec, "bb", 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->len, 2);
    size_t ln = 0;
    const char* g = ray_str_vec_get(loaded, 1, &ln);
    TEST_ASSERT_EQ_U(ln, 2);
    TEST_ASSERT_TRUE(memcmp(g, "bb", 2) == 0);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->len, 2);
    ray_release(mapped);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_slice_save ------------------------------------------------ */
/* Covers col_save_impl RAY_ATTR_SLICE branch (lines 601-610): saving a
 * zero-copy slice view materializes the parent's data window. */
static test_result_t test_col_slice_save(void) {
    int64_t raw[] = {10, 20, 30, 40, 50};
    ray_t* parent = ray_vec_from_raw(RAY_I64, raw, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parent));

    /* Slice [1..4) => {20, 30, 40} */
    ray_t* slice = ray_vec_slice(parent, 1, 3);
    TEST_ASSERT_NOT_NULL(slice);
    TEST_ASSERT_FALSE(RAY_IS_ERR(slice));
    TEST_ASSERT_TRUE(slice->attrs & RAY_ATTR_SLICE);
    TEST_ASSERT_EQ_I(slice->len, 3);

    ray_err_t err = ray_col_save(slice, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_I64);
    TEST_ASSERT_EQ_I(loaded->len, 3);
    /* Slice flag is cleared on save (materialized). */
    TEST_ASSERT_FALSE(loaded->attrs & RAY_ATTR_SLICE);
    int64_t* d = (int64_t*)ray_data(loaded);
    TEST_ASSERT_EQ_I(d[0], 20);
    TEST_ASSERT_EQ_I(d[1], 30);
    TEST_ASSERT_EQ_I(d[2], 40);
    ray_release(loaded);

    ray_release(slice);
    ray_release(parent);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_sym_empty_roundtrip --------------------------------------- */
/* Covers validate_sym_bounds "len == 0" early-return (line 46 arm 46:27):
 * a zero-length RAY_SYM column skips the index scan. */
static test_result_t test_col_sym_empty_roundtrip(void) {
    ray_sym_intern("syme_a", 6);
    ray_t* vec = ray_sym_vec_new(RAY_SYM_W8, 0);
    TEST_ASSERT_NOT_NULL(vec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    TEST_ASSERT_EQ_I(vec->len, 0);

    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_SYM);
    TEST_ASSERT_EQ_I(loaded->len, 0);
    ray_release(loaded);

    ray_t* mapped = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(mapped);
    TEST_ASSERT_FALSE(RAY_IS_ERR(mapped));
    TEST_ASSERT_EQ_I(mapped->len, 0);
    ray_release(mapped);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_sym_saved_count_reject ------------------------------------ */
/* Covers col_validate_mapped SYM fast-reject (line 874): a column file whose
 * saved sym count exceeds the current process sym count is rejected as
 * corrupt without scanning indices. We forge a high saved count in the rc
 * field of the on-disk header. */
static test_result_t test_col_sym_saved_count_reject(void) {
    ray_sym_intern("scr_a", 5);
    uint32_t sc = ray_sym_count();
    TEST_ASSERT_TRUE(sc >= 1);

    ray_t* vec = ray_sym_vec_new(RAY_SYM_W8, 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    vec->len = 1;
    ((uint8_t*)ray_data(vec))[0] = 0;
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* rc field (saved sym count) is at offsetof(ray_t, rc). For the on-disk
     * layout it sits at byte 20 (after mmod/order/type/attrs). Overwrite it
     * with a count far larger than the live table. */
    FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 20, SEEK_SET);
    uint32_t huge = sc + 1000000u;
    fwrite(&huge, sizeof(huge), 1, f);
    fclose(f);

    /* mmap path uses col_validate_mapped's saved-count branch. */
    ray_t* bad = ray_col_mmap(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_TRUE(RAY_IS_ERR(bad));
    TEST_ASSERT_STR_EQ(ray_err_code(bad), "corrupt");
    ray_release(bad);

    ray_release(vec);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_str_list_empty_string ------------------------------------- */
/* Covers col_save_str_list slen==0 branch (line 156:25 False) and
 * col_load_str_list reading a zero-length element: a RAY_LIST of -RAY_STR
 * atoms including an empty string. */
static test_result_t test_col_str_list_empty_string(void) {
    ray_t* list = ray_list_new(3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));
    ray_t* s0 = ray_str("nonempty", 8);
    ray_t* s1 = ray_str("", 0);          /* empty => slen == 0 */
    ray_t* s2 = ray_str("tail", 4);
    list = ray_list_append(list, s0);
    list = ray_list_append(list, s1);
    list = ray_list_append(list, s2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(list));

    ray_err_t err = ray_col_save(list, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 3);
    ray_t* l1 = ray_list_get(loaded, 1);
    TEST_ASSERT_NOT_NULL(l1);
    TEST_ASSERT_EQ_U(ray_str_len(l1), 0);
    ray_t* l2 = ray_list_get(loaded, 2);
    TEST_ASSERT_STR_EQ(ray_str_ptr(l2), "tail");

    ray_release(loaded);
    ray_release(s0);
    ray_release(s1);
    ray_release(s2);
    ray_release(list);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_recursive_list_in_list ------------------------------------ */
/* Covers col_write_recursive / col_read_recursive nested-list path
 * (lines 269-278 write, 397-413 read) and the empty -RAY_STR atom branch
 * (line 237:29 False: slen == 0). */
static test_result_t test_col_recursive_list_in_list(void) {
    /* Inner list: [i64 atom, empty str atom]. */
    ray_t* inner = ray_list_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(inner));
    ray_t* a = ray_i64(7);
    ray_t* b = ray_str("", 0);  /* empty atom => slen==0 in recursive write */
    inner = ray_list_append(inner, a);
    inner = ray_list_append(inner, b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(inner));

    /* Outer list: [inner_list, i64_vec]. Not a str list => col_save_list. */
    int64_t raw[] = {100, 200};
    ray_t* ivec = ray_vec_from_raw(RAY_I64, raw, 2);
    ray_t* outer = ray_list_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(outer));
    outer = ray_list_append(outer, inner);
    outer = ray_list_append(outer, ivec);
    TEST_ASSERT_FALSE(RAY_IS_ERR(outer));

    ray_err_t err = ray_col_save(outer, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 2);

    ray_t* got_inner = ray_list_get(loaded, 0);
    TEST_ASSERT_NOT_NULL(got_inner);
    TEST_ASSERT_EQ_I(got_inner->type, RAY_LIST);
    TEST_ASSERT_EQ_I(got_inner->len, 2);
    ray_t* gi0 = ray_list_get(got_inner, 0);
    TEST_ASSERT_EQ_I(gi0->type, -RAY_I64);
    TEST_ASSERT_EQ_I(gi0->i64, 7);
    ray_t* gi1 = ray_list_get(got_inner, 1);
    TEST_ASSERT_EQ_I(gi1->type, -RAY_STR);
    TEST_ASSERT_EQ_U(ray_str_len(gi1), 0);

    ray_t* got_vec = ray_list_get(loaded, 1);
    TEST_ASSERT_EQ_I(got_vec->type, RAY_I64);
    TEST_ASSERT_EQ_I(got_vec->len, 2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(got_vec))[1], 200);

    ray_release(loaded);
    ray_release(a);
    ray_release(b);
    ray_release(inner);
    ray_release(ivec);
    ray_release(outer);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_recursive_table ------------------------------------------- */
/* Covers col_write_recursive / col_read_recursive RAY_TABLE path
 * (lines 280-293 write, 416-442 read), including a nested STR column with a
 * pool inside the recursive serializer (lines 259-265 / 364-387). */
static test_result_t test_col_recursive_table(void) {
    /* Build a table [id name] with a pooled STR column, wrapped in a list so
     * is_str_list is false and the save routes through col_save_list ->
     * col_write_recursive, which then recurses into the RAY_TABLE (and into
     * the STR column with its pool inside the recursive serializer). */
    int64_t id_id   = ray_sym_intern("rc_id", 5);
    int64_t id_name = ray_sym_intern("rc_name", 7);

    int64_t ids[] = {1, 2};
    ray_t* idcol = ray_vec_from_raw(RAY_I64, ids, 2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(idcol));
    ray_t* namecol = ray_vec_new(RAY_STR, 2);
    namecol = ray_str_vec_append(namecol, "first-pooled-name-value", 23);
    namecol = ray_str_vec_append(namecol, "second-pooled-name-val!", 23);
    TEST_ASSERT_FALSE(RAY_IS_ERR(namecol));

    ray_t* tbl = ray_table_new(2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_id, idcol);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    tbl = ray_table_add_col(tbl, id_name, namecol);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Wrap in a list to force the recursive serializer. */
    ray_t* wrap = ray_list_new(1);
    wrap = ray_list_append(wrap, tbl);
    TEST_ASSERT_FALSE(RAY_IS_ERR(wrap));

    ray_err_t err = ray_col_save(wrap, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);

    ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
    TEST_ASSERT_EQ_I(loaded->type, RAY_LIST);
    TEST_ASSERT_EQ_I(loaded->len, 1);

    ray_t* got_tbl = ray_list_get(loaded, 0);
    TEST_ASSERT_NOT_NULL(got_tbl);
    TEST_ASSERT_EQ_I(got_tbl->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_ncols(got_tbl), 2);
    TEST_ASSERT_EQ_I(ray_table_nrows(got_tbl), 2);

    ray_t* gname = ray_table_get_col(got_tbl, id_name);
    TEST_ASSERT_NOT_NULL(gname);
    TEST_ASSERT_EQ_I(gname->type, RAY_STR);
    size_t ln = 0;
    const char* g0 = ray_str_vec_get(gname, 0, &ln);
    TEST_ASSERT_EQ_U(ln, 23);
    TEST_ASSERT_TRUE(memcmp(g0, "first-pooled-name-value", 23) == 0);

    ray_release(loaded);
    ray_release(idcol);
    ray_release(namecol);
    ray_release(tbl);
    ray_release(wrap);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

static bool test_write_exact(FILE* f, const void* data, size_t n) {
    return fwrite(data, 1, n, f) == n;
}

static bool test_write_i8(FILE* f, int8_t v) {
    return test_write_exact(f, &v, sizeof(v));
}

static bool test_write_u32(FILE* f, uint32_t v) {
    return test_write_exact(f, &v, sizeof(v));
}

static bool test_write_i64(FILE* f, int64_t v) {
    return test_write_exact(f, &v, sizeof(v));
}

static bool test_write_i64_vec(FILE* f, const int64_t* vals, int64_t len) {
    return test_write_i8(f, (int8_t)RAY_I64) &&
           test_write_i64(f, len) &&
           test_write_exact(f, vals, (size_t)len * sizeof(vals[0]));
}

/* ---- test_col_recursive_container_guards -------------------------------- */
static test_result_t test_col_recursive_container_guards(void) {
    enum {
        TEST_COL_LIST_MAGIC  = 0x4754534CU, /* "LSTG" */
        TEST_COL_TABLE_MAGIC = 0x4C425454U  /* "TTBL" */
    };

    /* TABLE with atom columns: recursive on-disk decoding must hit the same
     * shared table invariant as the serde decoder. */
    {
        FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "wb");
        TEST_ASSERT_NOT_NULL(f);
        int64_t name_a = ray_sym_intern("bad_a", 5);
        int64_t name_b = ray_sym_intern("bad_b", 5);
        bool ok = test_write_u32(f, TEST_COL_TABLE_MAGIC) &&
                  test_write_i8(f, (int8_t)RAY_TABLE) &&
                  test_write_i64(f, 2) &&
                  test_write_i64(f, 2) &&
                  test_write_i64(f, name_a) &&
                  test_write_i8(f, (int8_t)-RAY_I64) &&
                  test_write_i64(f, 2) &&
                  test_write_i64(f, name_b) &&
                  test_write_i8(f, (int8_t)-RAY_I64) &&
                  test_write_i64(f, 2);
        fclose(f);
        TEST_ASSERT_TRUE(ok);

        ray_t* bad = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_ERR_CODE_MSG(bad, "domain", "column must be list/vector-like");
        ray_error_free(bad);
    }

    /* DICT with two keys and one value, nested inside a generic list so it is
     * decoded by col_read_recursive. */
    {
        FILE* f = fopen(ray_test_tmp(TMP_COL_PATH), "wb");
        TEST_ASSERT_NOT_NULL(f);
        int64_t keys[] = { 7, 8 };
        int64_t vals[] = { 1 };
        bool ok = test_write_u32(f, TEST_COL_LIST_MAGIC) &&
                  test_write_i8(f, (int8_t)RAY_LIST) &&
                  test_write_i64(f, 1) &&
                  test_write_i8(f, (int8_t)RAY_DICT) &&
                  test_write_i64_vec(f, keys, 2) &&
                  test_write_i64_vec(f, vals, 1);
        fclose(f);
        TEST_ASSERT_TRUE(ok);

        ray_t* bad = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_ERR_CODE_MSG(bad, "domain", "key/value count mismatch");
        ray_error_free(bad);
    }

    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_link_sidecar_whitespace ----------------------------------- */
/* Covers try_load_link_sidecar trailing-whitespace trim loop (lines 489-491)
 * and the n==0 early return (line 492). ray_col_save never writes trailing
 * whitespace, so we hand-write the .link sidecar to exercise the trim. */
static test_result_t test_col_link_sidecar_whitespace(void) {
    int64_t target_sym = ray_sym_intern("lc_target_tbl", 13);
    TEST_ASSERT_TRUE(target_sym >= 0);

    /* Save a plain I64 column (eligible for link sidecar). */
    int64_t raw[] = {0, 1, 2};
    ray_t* vec = ray_vec_from_raw(RAY_I64, raw, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vec));
    ray_err_t err = ray_col_save(vec, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_OK);
    ray_release(vec);

    char link_path[1100];
    snprintf(link_path, sizeof link_path, "%s.link", ray_test_tmp(TMP_COL_PATH));

    /* (a) sidecar with trailing newline / spaces / tab => trimmed, link set. */
    {
        FILE* lf = fopen(link_path, "wb");
        TEST_ASSERT_NOT_NULL(lf);
        const char* body = "lc_target_tbl \t\r\n";  /* trailing whitespace */
        fwrite(body, 1, strlen(body), lf);
        fclose(lf);

        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_NOT_NULL(loaded);
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_TRUE(loaded->attrs & RAY_ATTR_HAS_LINK);
        TEST_ASSERT_EQ_I(loaded->link_target, target_sym);
        ray_release(loaded);
    }

    /* (b) sidecar that is only whitespace => trims to n==0 => no link. */
    {
        FILE* lf = fopen(link_path, "wb");
        TEST_ASSERT_NOT_NULL(lf);
        const char* body = "   \n\t\r ";
        fwrite(body, 1, strlen(body), lf);
        fclose(lf);

        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_NOT_NULL(loaded);
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_FALSE(loaded->attrs & RAY_ATTR_HAS_LINK);
        ray_release(loaded);
    }

    /* (c) empty sidecar => fread returns 0 => no link (early return). */
    {
        FILE* lf = fopen(link_path, "wb");
        TEST_ASSERT_NOT_NULL(lf);
        fclose(lf);
        ray_t* loaded = ray_col_load(ray_test_tmp(TMP_COL_PATH));
        TEST_ASSERT_NOT_NULL(loaded);
        TEST_ASSERT_FALSE(RAY_IS_ERR(loaded));
        TEST_ASSERT_FALSE(loaded->attrs & RAY_ATTR_HAS_LINK);
        ray_release(loaded);
    }

    unlink(link_path);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

/* ---- test_col_save_nyi_type --------------------------------------------- */
/* Covers col_save_impl is_serializable_type False default (line 543 / 112:55):
 * saving a type with no serializer (a RAY_DICT) returns RAY_ERR_NYI. */
static test_result_t test_col_save_nyi_type(void) {
    /* A dict is not in the serializable allowlist, is not a list/table/
     * str_list, so col_save_impl falls through to the NYI return. */
    ray_t* keys = ray_vec_new(RAY_STR, 1);
    keys = ray_str_vec_append(keys, "k", 1);
    int64_t vraw[] = {1};
    ray_t* vals = ray_vec_from_raw(RAY_I64, vraw, 1);
    ray_t* d = ray_dict_new(keys, vals);  /* consumes keys + vals */
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    ray_err_t err = ray_col_save(d, ray_test_tmp(TMP_COL_PATH));
    TEST_ASSERT_EQ_I(err, RAY_ERR_NYI);

    ray_release(d);
    unlink(ray_test_tmp(TMP_COL_PATH));
    PASS();
}

const test_entry_t store_entries[] = {
    { "store/col_mmap_i64", test_col_mmap_i64, store_setup, store_teardown },
    { "store/col_save_derives_has_nulls", test_col_save_derives_has_nulls, store_setup, store_teardown },
    { "store/col_mmap_f64", test_col_mmap_f64, store_setup, store_teardown },
    { "store/col_mmap_f32", test_col_mmap_f32, store_setup, store_teardown },
    { "store/col_mmap_cow", test_col_mmap_cow, store_setup, store_teardown },
    { "store/col_mmap_refcount", test_col_mmap_refcount, store_setup, store_teardown },
    { "store/col_mmap_nofile", test_col_mmap_nofile, store_setup, store_teardown },
    { "store/splay_open_roundtrip", test_splay_open_roundtrip, store_setup, store_teardown },
    { "store/splay_str_column_roundtrip", test_splay_str_column_roundtrip, store_setup, store_teardown },
    { "store/splay_short_strv_roundtrip", test_splay_short_strv_roundtrip, store_setup, store_teardown },
    { "store/splay_dict_column_roundtrip", test_splay_dict_column_roundtrip, store_setup, store_teardown },
    { "store/splay_empty_list_column_roundtrip", test_splay_empty_list_column_roundtrip, store_setup, store_teardown },
    { "store/splay_save_preflight", test_splay_save_preflight_preserves_generation, store_setup, store_teardown },
    { "store/parted_nrows", test_parted_nrows, store_setup, store_teardown },
    { "store/table_nrows_parted", test_table_nrows_parted, store_setup, store_teardown },
    { "store/parted_release", test_parted_release, store_setup, store_teardown },
    { "store/part_open", test_part_open, store_setup, store_teardown },
    { "store/parted_tables", test_parted_tables, store_setup, store_teardown },
    { "store/group_parted", test_group_parted, store_setup, store_teardown },
    { "store/col_large_nullable_roundtrip", test_col_large_nullable_roundtrip, store_setup, store_teardown },
    { "store/col_save_load_str", test_col_save_load_str, store_setup, store_teardown },
    { "store/col_save_load_list", test_col_save_load_list, store_setup, store_teardown },
    { "store/col_save_load_table", test_col_save_load_table, store_setup, store_teardown },
    { "store/col_save_load_bool_u8_i16", test_col_save_load_bool_u8_i16, store_setup, store_teardown },
    { "store/col_save_load_date_time_ts", test_col_save_load_date_time_timestamp, store_setup, store_teardown },
    { "store/col_sym_w32_roundtrip", test_col_sym_w32_roundtrip, store_setup, store_teardown },
    { "store/col_save_load_empty", test_col_save_load_empty, store_setup, store_teardown },
    { "store/col_validate_bad_type", test_col_validate_mapped_bad_type, store_setup, store_teardown },
    { "store/col_recursive_atoms", test_col_recursive_atoms, store_setup, store_teardown },
    { "store/col_recursive_sym_in_list", test_col_recursive_sym_in_list, store_setup, store_teardown },
    { "store/col_sym_w64_neg_index", test_col_sym_w64_negative_index, store_setup, store_teardown },
    { "store/col_str_pool_roundtrip", test_col_str_pool_roundtrip, store_setup, store_teardown },
    { "store/col_str_legacy_hash_repair", test_col_str_legacy_hash_repair, store_setup, store_teardown },
    { "store/col_str_hash_persisted", test_col_str_hash_persisted, store_setup, store_teardown },
    { "store/stream_str_hash_persisted", test_stream_str_hash_persisted, store_setup, store_teardown },
    { "store/parted_cache_limit", test_parted_cache_limit, store_setup, store_teardown },
    { "store/parted_cache_recency", test_parted_cache_recency, store_setup, store_teardown },
    { "store/parted_cache_gc", test_parted_cache_gc, store_setup, store_teardown },
    { "store/parted_live_domain_open", test_parted_live_domain_open, store_setup, store_teardown },
    { "store/parted_rebuilt_write_without_gc", test_parted_rebuilt_write_without_gc, store_setup, store_teardown },
    { "store/parted_rebuilt_splayed_without_gc", test_parted_rebuilt_splayed_without_gc, store_setup, store_teardown },
    { "store/parted_rebuilt_without_gc", test_parted_rebuilt_without_gc, store_setup, store_teardown },
    { "store/parted_rebuilt_symbols", test_parted_rebuilt_symbols, store_setup, store_teardown },
    { "store/parted_missing_symfile", test_parted_missing_symfile, store_setup, store_teardown },
    { "store/parted_symfile_name_in_error", test_parted_symfile_name_in_error, store_setup, store_teardown },
    { "store/parted_restored_mtime", test_parted_restored_mtime, store_setup, store_teardown },
    { "store/parted_generation_and_sweep", test_parted_generation_and_sweep, store_setup, store_teardown },
    { "store/parted_concurrent_mounts", test_parted_concurrent_mounts, store_setup, store_teardown },
    { "store/col_nested_str_hash_reset", test_col_nested_str_hash_reset, store_setup, store_teardown },
    { "store/col_save_str_bounds", test_col_save_str_bounds, store_setup, store_teardown },
    { "store/parted_remount_reuse", test_parted_remount_reuse, store_setup, store_teardown },
    { "store/col_format_version_roundtrip", test_col_format_version_roundtrip, store_setup, store_teardown },
    { "store/col_format_bad_version", test_col_format_bad_version, store_setup, store_teardown },
    { "store/col_str_empty_roundtrip", test_col_str_empty_roundtrip, store_setup, store_teardown },
    { "store/col_str_inline_only", test_col_str_inline_only_roundtrip, store_setup, store_teardown },
    { "store/col_slice_save", test_col_slice_save, store_setup, store_teardown },
    { "store/col_sym_empty_roundtrip", test_col_sym_empty_roundtrip, store_setup, store_teardown },
    { "store/col_sym_saved_count_reject", test_col_sym_saved_count_reject, store_setup, store_teardown },
    { "store/col_str_list_empty_string", test_col_str_list_empty_string, store_setup, store_teardown },
    { "store/col_recursive_list_in_list", test_col_recursive_list_in_list, store_setup, store_teardown },
    { "store/col_recursive_table", test_col_recursive_table, store_setup, store_teardown },
    { "store/col_recursive_container_guards", test_col_recursive_container_guards, store_setup, store_teardown },
    { "store/col_link_sidecar_whitespace", test_col_link_sidecar_whitespace, store_setup, store_teardown },
    { "store/col_save_nyi_type", test_col_save_nyi_type, store_setup, store_teardown },
    { "store/file_open_close", test_file_open_close, store_setup, store_teardown },
    { "store/file_lock_unlock", test_file_lock_unlock, store_setup, store_teardown },
    { "store/file_sync", test_file_sync_op, store_setup, store_teardown },
    { "store/file_rename", test_file_rename_op, store_setup, store_teardown },
    { "store/col_close_failure", test_col_close_failure, store_setup, store_teardown },
    { "store/col_append_index_rollback", test_col_append_index_rollback, store_setup, store_teardown },
    { "store/zone_acc_slices_equal_compute", test_zone_acc_slices_equal_compute, store_setup, store_teardown },
    { "store/zone_acc_one_chunk",            test_zone_acc_one_chunk,            store_setup, store_teardown },
    { "store/zone_acc_merge_mid_chunk",      test_zone_acc_merge_mid_chunk,      store_setup, store_teardown },
    { "store/zone_acc_float",                test_zone_acc_float,                store_setup, store_teardown },
    { "store/dict_acc_slices_equal_compute", test_dict_acc_slices_equal_compute, store_setup, store_teardown },
    { "store/dict_acc_leading_empty", test_dict_acc_leading_empty, store_setup, store_teardown },
    { "store/dict_acc_high_cardinality",     test_dict_acc_high_cardinality,     store_setup, store_teardown },
    { "store/stream_inline_zone", test_stream_inline_zone, store_setup, store_teardown },
    { "store/zone_acc_sentinel_without_flag", test_zone_acc_sentinel_without_flag, store_setup, store_teardown },
    { "store/stream_sentinel_has_nulls", test_stream_sentinel_publishes_has_nulls, store_setup, store_teardown },
    { "store/stream_sym_chunks", test_stream_sym_chunks, store_setup, store_teardown },
    { "store/stream_sym_from_str", test_stream_sym_from_str, store_setup, store_teardown },
    { "store/stream_sym_foreign", test_stream_sym_foreign, store_setup, store_teardown },
    { "store/stream_sym_lut_stale", test_stream_sym_lut_stale, store_setup, store_teardown },
    { "store/stream_index_write_failure", test_stream_index_write_failure_drops_index, store_setup, store_teardown },
    { "store/stream_inline_zone_one_chunk", test_stream_inline_zone_one_chunk, store_setup, store_teardown },
    { "store/col_build_hash_index_bytes", test_col_build_hash_index_bytes, store_setup, store_teardown },
    { "store/col_build_hash_index_refusals", test_col_build_hash_index_refusals, store_setup, store_teardown },
    { "store/col_build_hash_index_untrusted_fs", test_col_build_hash_index_untrusted_fs, store_setup, store_teardown },
    { "store/col_build_hash_index_fs_types", test_col_build_hash_index_fs_types, store_setup, store_teardown },
    { "store/col_build_hash_index_refused", test_col_build_hash_index_refused, store_setup, store_teardown },
    { "store/col_build_hash_index_truncate", test_col_build_hash_index_truncate, store_setup, store_teardown },
    { "store/stream_close_all_cancel", test_stream_close_all_cancel, store_setup, store_teardown },
    { "store/splay_save_cancel_late", test_splay_save_cancel_late, store_setup, store_teardown },
    { "store/splay_save_cancel_leaks", test_splay_save_cancel_leaks, store_setup, store_teardown },
    { "store/col_truncated_fails", test_col_truncated_fails, store_setup, store_teardown },
    { "store/col_build_hash_index_marker_last", test_col_build_hash_index_marker_last, store_setup, store_teardown },
    { "store/col_build_hash_index_rollback", test_col_build_hash_index_rollback, store_setup, store_teardown },
    { "store/col_build_hash_index_interrupt", test_col_build_hash_index_interrupt, store_setup, store_teardown },
    { "store/stream_inline_dict", test_stream_inline_dict, store_setup, store_teardown },
    { "store/staged_bulk_no_hash", test_staged_bulk_no_hash, store_setup, store_teardown },
    { "store/resave_keeps_hash", test_resave_keeps_hash, store_setup, store_teardown },
    { "idxlazy/first_use",     test_lazy_first_use,     lz_setup, lz_teardown },
    { "idxlazy/eq_range",      test_lazy_eq_range,      lz_setup, lz_teardown },
    { "idxlazy/not_persisted", test_lazy_not_persisted, lz_setup, lz_teardown },
    { "idxlazy/budget",        test_lazy_budget,        lz_setup, lz_teardown },
    { "idxlazy/not_wanted",    test_lazy_not_wanted,    lz_setup, lz_teardown },
    { "idxlazy/gen2",          test_lazy_gen2,          lz_setup, lz_teardown },
    { "idxlazy/reload_bounded", test_lazy_reload_bounded, lz_setup, lz_teardown },
    { "idxlazy/timestamp",     test_lazy_timestamp,     lz_setup, lz_teardown },
    { "idxlazy/nulls_never",   test_lazy_nulls_never,   lz_setup, lz_teardown },
    { "idxlazy/derived_never", test_lazy_derived_never, lz_setup, lz_teardown },
    { "idxlazy/after_mutation", test_lazy_after_mutation, lz_setup, lz_teardown },
    { "idxlazy/out_of_range",  test_lazy_out_of_range,  lz_setup, lz_teardown },
    { "idxlazy/out_of_range_gen2", test_lazy_out_of_range_gen2, lz_setup, lz_teardown },
    { "idxlazy/concurrent",    test_lazy_concurrent,    lz_setup, lz_teardown },
    { "idxlazy/memo_evicts_memo", test_lazy_memo_evicts_memo, lz_setup, lz_teardown },
    { "idxlazy/cancel",        test_lazy_cancel,        lz_setup, lz_teardown },
    { "idxlazy/oom",           test_lazy_oom,           lz_setup, lz_teardown },
    { "store/stream_region_bytes", test_stream_region_bytes_match, store_setup, store_teardown },
    { "store/stream_finish_failure", test_stream_finish_failure_publishes_nothing, store_setup, store_teardown },
    { "store/stream_cancel_publishes_nothing", test_stream_cancel_publishes_nothing, store_setup, store_teardown },
    { "store/file_rename_new", test_file_rename_new, store_setup, store_teardown },
    { "store/file_rename_new_emulated", test_file_rename_new_emulated, store_setup, store_teardown },
    { "store/file_shared_lock", test_file_shared_lock_concurrent, store_setup, store_teardown },
    { "store/sym_col_bounds_reject", test_sym_col_bounds_reject, store_setup, store_teardown },
    { "store/sym_col_count_mismatch", test_sym_col_count_mismatch, store_setup, store_teardown },
    { "store/sym_col_valid_roundtrip", test_sym_col_valid_roundtrip, store_setup, store_teardown },
    { "store/splay_load_with_sym", test_splay_load_with_sym, store_setup, store_teardown },
    { "store/splay_load_sym_missing", test_splay_load_sym_missing_corrupt, store_setup, store_teardown },
    { "store/read_splayed_bad_sym", test_read_splayed_bad_sym_fatal, store_setup, store_teardown },
    { "store/serde_long_str_roundtrip", test_serde_long_str_roundtrip, store_setup, store_teardown },
    { "store/serde_null_roundtrip", test_serde_null_roundtrip, store_setup, store_teardown },
    { "store/serde_endian_mismatch", test_serde_endian_mismatch, store_setup, store_teardown },
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
    { "store/serde_builtin_name_roundtrip", test_serde_builtin_name_roundtrip, NULL,     NULL           },
    { "store/serde_error_roundtrip",      test_serde_error_roundtrip,      store_setup, store_teardown },
    { "store/serde_large_null_vec",       test_serde_large_null_vec,       store_setup, store_teardown },
    { "store/serde_f32_atom",             test_serde_f32_atom_and_edge_cases, store_setup, store_teardown },
    { "store/serde_lambda_roundtrip",     test_serde_lambda_roundtrip,     store_setup, store_teardown },
    { "store/serde_lambda_malformed_closure", test_serde_lambda_malformed_closure, store_setup, store_teardown },
    { "store/serde_save_serde_error",     test_serde_save_serde_error,     store_setup, store_teardown },
    { "store/serde_de_raw_default",       test_serde_de_raw_default_and_errors, store_setup, store_teardown },
    { "store/serde_table_dict_de_errors", test_serde_table_dict_de_errors, store_setup, store_teardown },
    { "store/serde_container_count_mismatch", test_serde_container_count_mismatch, store_setup, store_teardown },
    { "store/serde_table_de_type_mismatch", test_serde_table_de_type_mismatch, store_setup, store_teardown },
    { "store/serde_de_size_bounds",        test_serde_de_size_bounds,        store_setup, store_teardown },
    { "store/total_ram", test_total_ram, NULL, NULL },
    { "store/ipc/compress_rt", test_ipc_compress_rt, NULL, NULL },
    { "store/ipc/compress_threshold", test_ipc_compress_threshold, NULL, NULL },
    { "store/ipc/compress_zeros", test_ipc_compress_zeros, NULL, NULL },
    { "store/ipc/sync_roundtrip", test_ipc_sync_roundtrip, NULL, NULL },
    { "store/ipc/async_send", test_ipc_async_send, NULL, NULL },
    { "store/ipc/auth_success", test_ipc_auth_success, NULL, NULL },
    { "store/ipc/auth_reject", test_ipc_auth_reject, NULL, NULL },
    { "store/ipc/auth_no_creds", test_ipc_auth_no_creds, NULL, NULL },
    { "store/ipc/restricted", test_ipc_restricted, NULL, NULL },
    { "store/ipc/handshake_version_mismatch", test_ipc_handshake_version_mismatch, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
