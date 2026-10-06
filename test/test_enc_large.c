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
#include "mem/heap.h"
#include "table/sym.h"
#include "store/col.h"
#include "store/enccol.h"
#include "store/stream.h"
#include "vec/str.h"
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define LG_TMP "/tmp/rayforce_enc_large"
static void lg_setup(void)    { ray_heap_init(); (void)ray_sym_init(); (void)ray_test_mkdir_p(LG_TMP); }
static void lg_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); (void)ray_test_rm_rf(LG_TMP); }

/* 65535 descriptors sharing one pooled range of the source (a 65538-byte
 * pool): the source stays small, while the decoded compact pool counts each
 * occurrence (spec "STR pool", rule 2).  65535 * 65537 = UINT32_MAX exactly;
 * a last row of 65538 bytes makes it one byte more.  The chunk (descriptors
 * plus per-vector pools) exceeds 4 GiB, so `u64` sizes are exercised too. */
static ray_t* shared_str(uint32_t last_len) {
    ray_t* v = ray_vec_new(RAY_STR, 65535);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = 65535;
    ray_t* pool = ray_alloc(65538);
    if (!pool || RAY_IS_ERR(pool)) { ray_release(v); return pool; }
    pool->type = RAY_U8; pool->len = 65538;
    memset(ray_data(pool), 'q', 65538);
    v->str_pool = pool;
    ray_str_t* d = ray_data(v);
    for (int i = 0; i < 65535; i++) {
        memset(&d[i], 0, sizeof d[i]);
        d[i].len = i == 65534 ? last_len : 65537;
        memcpy(d[i].prefix, "qqqq", 4);
        d[i].pool_off = 0;
    }
    return v;
}

static test_result_t test_pool_exact_limit(void) {
    ray_t* v = shared_str(65537);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, LG_TMP "/ok", 2), RAY_OK);
    ray_release(v);
    ray_t* c = ray_col_mmap(LG_TMP "/ok");
    TEST_ASSERT_TRUE(c && !RAY_IS_ERR(c));
    TEST_ASSERT_EQ_U(ray_enccol_of(c)->hdr.str_pool_bytes, UINT32_MAX);
    ray_t* m = ray_enccol_materialize(c);
    TEST_ASSERT_TRUE(m && !RAY_IS_ERR(m));
    TEST_ASSERT_EQ_I(m->str_pool->len, (int64_t)UINT32_MAX);
    ray_release(c);
    PASS();
}

static test_result_t test_pool_one_byte_over(void) {
    ray_t* v = shared_str(65538);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_col_save_bulk_gen(v, LG_TMP "/over", 2), RAY_ERR_RANGE);
    ray_release(v);
    TEST_ASSERT_TRUE(access(LG_TMP "/over", F_OK) != 0);
    TEST_ASSERT_TRUE(access(LG_TMP "/over.tmp", F_OK) != 0);
    PASS();
}

/* Streamed: the same shared-range vector in one append; the generation-2
 * stream counts the decoded pool per occurrence and fails with range at
 * the crossing (the last) row; abort removes every staging file. */
static test_result_t test_stream_crossing_last_row(void) {
    ray_col_stream_t w;
    TEST_ASSERT_EQ_I(ray_col_stream_open(&w, LG_TMP, ray_sym_intern("st", 2), RAY_STR, NULL, 2), RAY_OK);
    ray_t* v = shared_str(65538);
    TEST_ASSERT_TRUE(v && !RAY_IS_ERR(v));
    TEST_ASSERT_EQ_I(ray_col_stream_append(&w, v), RAY_ERR_RANGE);
    ray_release(v);
    ray_col_stream_abort(&w);
    TEST_ASSERT_TRUE(access(LG_TMP "/.stream-st.col", F_OK) != 0);
    TEST_ASSERT_TRUE(access(LG_TMP "/.stream-st.pool", F_OK) != 0);
    PASS();
}

const test_entry_t large_entries[] = {
    { "large/pool_exact_limit",         test_pool_exact_limit,         lg_setup, lg_teardown },
    { "large/pool_one_byte_over",       test_pool_one_byte_over,       lg_setup, lg_teardown },
    { "large/stream_crossing_last_row", test_stream_crossing_last_row, lg_setup, lg_teardown },
    { NULL, NULL, NULL, NULL },
};
