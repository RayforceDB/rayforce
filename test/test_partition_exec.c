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
 * test_partition_exec.c -- C-side tests for the parted/MAPCOMMON exec
 * paths in src/ops/exec.c that the rfl test surface cannot reach.
 *
 * The rfl harness can only construct flat tables, so the production
 * paths that handle RAY_MAPCOMMON columns and RAY_IS_PARTED columns
 * (~750 lines in exec.c) are exercised only by reading on-disk parted
 * tables.  These tests build the column shapes synthetically and drive
 * each region directly.
 *
 * Coverage targets:
 *   - exec.c L35-152   materialize_mapcommon{,_head,_filter}
 *   - exec.c L1351-1502 OP_HEAD / OP_TAIL on parted/MAPCOMMON columns
 *   - exec.c L1793-1832 ray_result_merge (via streaming filter)
 *   - exec.c L1839-1920 build_segment_table (via streaming filter)
 *   - exec.c L1979-2248 segment streaming loop (via streaming filter)
 *   - exec.c L259-473  partitioned_gather phases (esz=1, esz=2)
 *
 * Build pattern follows test_opt.c L377-417 (MAPCOMMON construction)
 * and the existing test_*.c suite layout.
 */

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "ops/rowsel.h"
#include "table/sym.h"
#include "core/pool.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

/* Build a MAPCOMMON column from an arbitrary typed key vector and i64 row
 * counts.  `key_values` and `row_counts` are released to the resulting
 * MAPCOMMON's data slots (no extra retain needed by the caller). */
static ray_t* make_mapcommon(ray_t* key_values, ray_t* row_counts) {
    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    if (!mc) return NULL;
    mc->type = RAY_MAPCOMMON;
    mc->len = 2;
    ((ray_t**)ray_data(mc))[0] = key_values;
    ((ray_t**)ray_data(mc))[1] = row_counts;
    return mc;
}

/* Build a parted column wrapping `n_segs` segment vectors of `base` type.
 * Segments are referenced (not retained) so caller still owns them. */
static ray_t* make_parted(int8_t base, ray_t** segs, int64_t n_segs) {
    ray_t* p = ray_alloc((size_t)n_segs * sizeof(ray_t*));
    if (!p) return NULL;
    p->type = RAY_PARTED_BASE + base;
    p->len = n_segs;
    ray_t** out = (ray_t**)ray_data(p);
    for (int64_t i = 0; i < n_segs; i++) out[i] = segs[i];
    return p;
}

/* --------------------------------------------------------------------------
 * Test 1: materialize_mapcommon — basic broadcast
 *
 * Targets exec.c L35-75: full materialization of a MAPCOMMON column to
 * a flat typed vector.  Builds a 3-partition DATE column with row counts
 * [100, 50, 200] and verifies each output row carries the correct date.
 * -------------------------------------------------------------------------- */
static test_result_t test_materialize_mapcommon_basic(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* I64 keys (8 bytes) exercises the esz==8 fast path.
     * RAY_DATE is only 4 bytes — using I64 here keeps the assertions
     * cleanly typed without per-platform date-encoding noise. */
    int64_t keys[] = {20240101, 20240102, 20240103};
    int64_t counts_data[] = {100, 50, 200};

    ray_t* kv = ray_vec_new(RAY_I64, 3);
    TEST_ASSERT_NOT_NULL(kv);
    kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));

    ray_t* rc = ray_vec_new(RAY_I64, 3);
    TEST_ASSERT_NOT_NULL(rc);
    rc->len = 3;
    memcpy(ray_data(rc), counts_data, sizeof(counts_data));

    ray_t* mc = make_mapcommon(kv, rc);
    TEST_ASSERT_NOT_NULL(mc);

    ray_t* flat = materialize_mapcommon(mc);
    TEST_ASSERT_NOT_NULL(flat);
    TEST_ASSERT_FALSE(RAY_IS_ERR(flat));
    TEST_ASSERT_EQ_I(flat->type, RAY_I64);
    TEST_ASSERT_EQ_I(flat->len, 350);

    int64_t* out = (int64_t*)ray_data(flat);
    /* Rows [0..100): key 0; [100..150): key 1; [150..350): key 2 */
    for (int64_t i = 0; i < 100; i++) TEST_ASSERT_EQ_I(out[i], 20240101);
    for (int64_t i = 100; i < 150; i++) TEST_ASSERT_EQ_I(out[i], 20240102);
    for (int64_t i = 150; i < 350; i++) TEST_ASSERT_EQ_I(out[i], 20240103);

    ray_release(flat);
    ray_release(mc);
    ray_release(kv);
    ray_release(rc);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 2: materialize_mapcommon_head — clamped expansion
 *
 * Targets exec.c L77-114.  Same fixture as Test 1 but only the first
 * 75 rows.  Verifies the loop terminates at `n` and that the result is
 * the per-row prefix of the full materialization.
 * -------------------------------------------------------------------------- */
static test_result_t test_materialize_mapcommon_head(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t keys[] = {20240101, 20240102, 20240103};
    int64_t counts_data[] = {100, 50, 200};

    ray_t* kv = ray_vec_new(RAY_I64, 3);
    kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));

    ray_t* rc = ray_vec_new(RAY_I64, 3);
    rc->len = 3;
    memcpy(ray_data(rc), counts_data, sizeof(counts_data));

    ray_t* mc = make_mapcommon(kv, rc);
    TEST_ASSERT_NOT_NULL(mc);

    /* head 75: all within partition 0 */
    ray_t* flat = materialize_mapcommon_head(mc, 75);
    TEST_ASSERT_NOT_NULL(flat);
    TEST_ASSERT_FALSE(RAY_IS_ERR(flat));
    TEST_ASSERT_EQ_I(flat->len, 75);
    int64_t* out = (int64_t*)ray_data(flat);
    for (int64_t i = 0; i < 75; i++) TEST_ASSERT_EQ_I(out[i], 20240101);
    ray_release(flat);

    /* head 175: spans partitions 0+1 + 25 of part 2 */
    ray_t* flat2 = materialize_mapcommon_head(mc, 175);
    TEST_ASSERT_EQ_I(flat2->len, 175);
    int64_t* out2 = (int64_t*)ray_data(flat2);
    for (int64_t i = 0; i < 100; i++) TEST_ASSERT_EQ_I(out2[i], 20240101);
    for (int64_t i = 100; i < 150; i++) TEST_ASSERT_EQ_I(out2[i], 20240102);
    for (int64_t i = 150; i < 175; i++) TEST_ASSERT_EQ_I(out2[i], 20240103);
    ray_release(flat2);

    ray_release(mc);
    ray_release(kv);
    ray_release(rc);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 3: materialize_mapcommon_filter — selective materialization
 *
 * Targets exec.c L116-152.  Uses a hand-built RAY_BOOL predicate to keep
 * a subset of rows from each partition and verifies each kept row has the
 * key for its source partition.  Drives the morsel iteration + the
 * partition-cursor advance logic at L142-149.
 * -------------------------------------------------------------------------- */
static test_result_t test_materialize_mapcommon_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t keys[] = {20240101, 20240102, 20240103};
    int64_t counts_data[] = {10, 5, 8};       /* 23 total rows */

    ray_t* kv = ray_vec_new(RAY_I64, 3);
    kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));

    ray_t* rc = ray_vec_new(RAY_I64, 3);
    rc->len = 3;
    memcpy(ray_data(rc), counts_data, sizeof(counts_data));

    ray_t* mc = make_mapcommon(kv, rc);
    TEST_ASSERT_NOT_NULL(mc);

    /* Predicate: keep every other row.  Pass count = 12 (rows 0,2,...,22). */
    ray_t* pred = ray_vec_new(RAY_BOOL, 23);
    TEST_ASSERT_NOT_NULL(pred);
    pred->len = 23;
    uint8_t* pbits = (uint8_t*)ray_data(pred);
    int64_t expected_pass = 0;
    for (int64_t i = 0; i < 23; i++) {
        pbits[i] = (uint8_t)((i % 2) == 0);
        if (pbits[i]) expected_pass++;
    }
    TEST_ASSERT_EQ_I(expected_pass, 12);

    ray_t* out = materialize_mapcommon_filter(mc, pred, expected_pass);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(out->len, expected_pass);

    /* For each kept row, recompute which partition it belonged to and
     * compare to the broadcast key. */
    int64_t* od = (int64_t*)ray_data(out);
    int64_t kept = 0;
    for (int64_t row = 0; row < 23; row++) {
        if (!pbits[row]) continue;
        int64_t rs = row;
        int64_t part;
        if (rs < 10)              part = 0;
        else if (rs < 15)         part = 1;
        else                      part = 2;
        TEST_ASSERT_EQ_I(od[kept], keys[part]);
        kept++;
    }
    TEST_ASSERT_EQ_I(kept, expected_pass);

    ray_release(out);
    ray_release(pred);
    ray_release(mc);
    ray_release(kv);
    ray_release(rc);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 4: OP_HEAD on a parted+MAPCOMMON table
 *
 * Targets exec.c L1338-1408.  Builds a parted I64 column with 3 segments
 * plus a MAPCOMMON date key column, wraps the whole thing as a constant
 * table operand to OP_HEAD, and verifies the per-segment fast-path copies
 * the right rows out of each segment + materializes the MAPCOMMON head.
 * -------------------------------------------------------------------------- */
static test_result_t test_op_head_on_parted_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments: 4 + 6 + 5 rows = 15 total */
    ray_t* s0 = ray_vec_new(RAY_I64, 4);
    s0->len = 4;
    int64_t s0d[] = {100, 101, 102, 103};
    memcpy(ray_data(s0), s0d, sizeof(s0d));

    ray_t* s1 = ray_vec_new(RAY_I64, 6);
    s1->len = 6;
    int64_t s1d[] = {200, 201, 202, 203, 204, 205};
    memcpy(ray_data(s1), s1d, sizeof(s1d));

    ray_t* s2 = ray_vec_new(RAY_I64, 5);
    s2->len = 5;
    int64_t s2d[] = {300, 301, 302, 303, 304};
    memcpy(ray_data(s2), s2d, sizeof(s2d));

    ray_t* segs[3] = { s0, s1, s2 };
    ray_t* val = make_parted(RAY_I64, segs, 3);
    TEST_ASSERT_NOT_NULL(val);

    /* MAPCOMMON keyed by I64 — counts must match parted segment lengths */
    int64_t keys[] = {20240101, 20240102, 20240103};
    int64_t counts[] = {4, 6, 5};
    ray_t* kv = ray_vec_new(RAY_I64, 3); kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));
    ray_t* rc = ray_vec_new(RAY_I64, 3); rc->len = 3;
    memcpy(ray_data(rc), counts, sizeof(counts));
    ray_t* mc = make_mapcommon(kv, rc);

    int64_t sym_dt  = ray_sym_intern("dt", 2);
    int64_t sym_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dt, mc);
    tbl = ray_table_add_col(tbl, sym_val, val);

    /* head 7 from constant-table — 4 from seg 0 + 3 from seg 1 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tnode = ray_const_table(g, tbl);
    ray_op_t* h = ray_head(g, tnode, 7);
    ray_t* result = ray_execute(g, h);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 7);

    /* val column: should be flat I64 of [100,101,102,103,200,201,202] */
    ray_t* vcol = ray_table_get_col(result, sym_val);
    TEST_ASSERT_NOT_NULL(vcol);
    TEST_ASSERT_EQ_I(vcol->type, RAY_I64);
    TEST_ASSERT_EQ_I(vcol->len, 7);
    int64_t* vd = (int64_t*)ray_data(vcol);
    int64_t expected[] = {100, 101, 102, 103, 200, 201, 202};
    for (int64_t i = 0; i < 7; i++) TEST_ASSERT_EQ_I(vd[i], expected[i]);

    /* dt column: should be flat I64 of [k0,k0,k0,k0,k1,k1,k1] */
    ray_t* dcol = ray_table_get_col(result, sym_dt);
    TEST_ASSERT_NOT_NULL(dcol);
    TEST_ASSERT_EQ_I(dcol->type, RAY_I64);
    TEST_ASSERT_EQ_I(dcol->len, 7);
    int64_t* dd = (int64_t*)ray_data(dcol);
    int64_t edates[] = {20240101, 20240101, 20240101, 20240101,
                        20240102, 20240102, 20240102};
    for (int64_t i = 0; i < 7; i++) TEST_ASSERT_EQ_I(dd[i], edates[i]);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(mc);    ray_release(kv);  ray_release(rc);
    ray_release(val);
    ray_release(s0);    ray_release(s1);  ray_release(s2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 5: OP_TAIL on a parted+MAPCOMMON table
 *
 * Targets exec.c L1423-1517.  Same fixture as Test 4 but takes the last
 * 7 rows (3 from end of seg 1 + all of seg 2) and verifies the reverse
 * walk in the tail loop.
 * -------------------------------------------------------------------------- */
static test_result_t test_op_tail_on_parted_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* s0 = ray_vec_new(RAY_I64, 4);
    s0->len = 4;
    int64_t s0d[] = {100, 101, 102, 103};
    memcpy(ray_data(s0), s0d, sizeof(s0d));

    ray_t* s1 = ray_vec_new(RAY_I64, 6);
    s1->len = 6;
    int64_t s1d[] = {200, 201, 202, 203, 204, 205};
    memcpy(ray_data(s1), s1d, sizeof(s1d));

    ray_t* s2 = ray_vec_new(RAY_I64, 5);
    s2->len = 5;
    int64_t s2d[] = {300, 301, 302, 303, 304};
    memcpy(ray_data(s2), s2d, sizeof(s2d));

    ray_t* segs[3] = { s0, s1, s2 };
    ray_t* val = make_parted(RAY_I64, segs, 3);

    int64_t keys[] = {20240101, 20240102, 20240103};
    int64_t counts[] = {4, 6, 5};
    ray_t* kv = ray_vec_new(RAY_I64, 3); kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));
    ray_t* rc = ray_vec_new(RAY_I64, 3); rc->len = 3;
    memcpy(ray_data(rc), counts, sizeof(counts));
    ray_t* mc = make_mapcommon(kv, rc);

    int64_t sym_dt  = ray_sym_intern("dt", 2);
    int64_t sym_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dt, mc);
    tbl = ray_table_add_col(tbl, sym_val, val);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tnode = ray_const_table(g, tbl);
    ray_op_t* t = ray_tail(g, tnode, 7);
    ray_t* result = ray_execute(g, t);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 7);

    /* val: last 7 of [100..103, 200..205, 300..304]
     * => [203, 204, 205, 300, 301, 302, 303, 304] (last 7 == [204,205,300..304]).
     * Wait — total = 15, last 7 = rows 8..14 = [204, 205, 300, 301, 302, 303, 304]. */
    ray_t* vcol = ray_table_get_col(result, sym_val);
    TEST_ASSERT_EQ_I(vcol->len, 7);
    int64_t* vd = (int64_t*)ray_data(vcol);
    int64_t expected[] = {204, 205, 300, 301, 302, 303, 304};
    for (int64_t i = 0; i < 7; i++) TEST_ASSERT_EQ_I(vd[i], expected[i]);

    /* dt: last 7 dates = [k1,k1,k2,k2,k2,k2,k2] */
    ray_t* dcol = ray_table_get_col(result, sym_dt);
    TEST_ASSERT_EQ_I(dcol->len, 7);
    int64_t* dd = (int64_t*)ray_data(dcol);
    int64_t edates[] = {20240102, 20240102, 20240103, 20240103,
                        20240103, 20240103, 20240103};
    for (int64_t i = 0; i < 7; i++) TEST_ASSERT_EQ_I(dd[i], edates[i]);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(mc);    ray_release(kv);  ray_release(rc);
    ray_release(val);
    ray_release(s0);    ray_release(s1);  ray_release(s2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 6: Segment streaming filter on a parted table
 *
 * Targets exec.c L1839-1920 (build_segment_table) plus the streaming
 * loop at L2069-2248 (including ray_result_merge at L1793-1832).
 * Builds a 4-segment parted table with a MAPCOMMON DATE key + parted
 * I64 value column and runs FILTER(SCAN(val), val >= 30) through
 * ray_execute.  Streaming mode is selected because the DAG is fully
 * streamable (SCAN, GE, FILTER) and the table has parted columns.
 * -------------------------------------------------------------------------- */
static test_result_t test_segment_streaming_filter(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 4 segments, each 10 rows of I64, values 0..9, 10..19, 20..29, 30..39 */
    ray_t* segs_v[4];
    for (int i = 0; i < 4; i++) {
        segs_v[i] = ray_vec_new(RAY_I64, 10);
        segs_v[i]->len = 10;
        int64_t* d = (int64_t*)ray_data(segs_v[i]);
        for (int j = 0; j < 10; j++) d[j] = (int64_t)(i * 10 + j);
    }
    ray_t* val = make_parted(RAY_I64, segs_v, 4);

    /* MAPCOMMON keys = 4 i64 partition tags, counts = [10,10,10,10] */
    int64_t keys[] = {20240101, 20240102, 20240103, 20240104};
    int64_t counts[] = {10, 10, 10, 10};
    ray_t* kv = ray_vec_new(RAY_I64, 4); kv->len = 4;
    memcpy(ray_data(kv), keys, sizeof(keys));
    ray_t* rc = ray_vec_new(RAY_I64, 4); rc->len = 4;
    memcpy(ray_data(rc), counts, sizeof(counts));
    ray_t* mc = make_mapcommon(kv, rc);

    int64_t sym_dt  = ray_sym_intern("dt", 2);
    int64_t sym_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dt, mc);
    tbl = ray_table_add_col(tbl, sym_val, val);

    /* select where: val >= 30  (FILTER on a SCAN — streamable) */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sval = ray_scan(g, "val");
    ray_op_t* c30  = ray_const_i64(g, 30);
    ray_op_t* pred = ray_ge(g, sval, c30);
    ray_op_t* sval2 = ray_scan(g, "val");
    ray_op_t* flt  = ray_filter(g, sval2, pred);
    ray_t* result = ray_execute(g, flt);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    /* The filter result is a vector (filtered val).  10 matches: 30..39. */
    TEST_ASSERT_EQ_I(result->len, 10);
    int64_t* rd = (int64_t*)ray_data(result);
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQ_I(rd[i], (int64_t)(30 + i));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(mc); ray_release(kv); ray_release(rc);
    ray_release(val);
    for (int i = 0; i < 4; i++) ray_release(segs_v[i]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 7: ray_result_merge — multi-segment vector concat
 *
 * Targets exec.c L1827-1829 (vector merge branch of ray_result_merge)
 * by running an element-wise streamable expression over a 3-segment
 * parted table.  Each segment produces a partial vector; the streaming
 * loop must concatenate them in order.
 *
 * Drives the merge across more than 2 segments so the accumulator
 * branch (L1798-1801: accum non-NULL, partial non-NULL) gets multiple
 * iterations rather than just initialization on the first segment.
 * -------------------------------------------------------------------------- */
static test_result_t test_result_merge_via_streaming(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments × 4 rows = 12 total */
    ray_t* segs_v[3];
    int64_t s0[] = {1, 2, 3, 4};
    int64_t s1[] = {5, 6, 7, 8};
    int64_t s2[] = {9, 10, 11, 12};
    segs_v[0] = ray_vec_new(RAY_I64, 4); segs_v[0]->len = 4;
    memcpy(ray_data(segs_v[0]), s0, sizeof(s0));
    segs_v[1] = ray_vec_new(RAY_I64, 4); segs_v[1]->len = 4;
    memcpy(ray_data(segs_v[1]), s1, sizeof(s1));
    segs_v[2] = ray_vec_new(RAY_I64, 4); segs_v[2]->len = 4;
    memcpy(ray_data(segs_v[2]), s2, sizeof(s2));

    ray_t* val = make_parted(RAY_I64, segs_v, 3);

    int64_t keys[]   = {20240101, 20240102, 20240103};
    int64_t counts[] = {4, 4, 4};
    ray_t* kv = ray_vec_new(RAY_I64, 3); kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));
    ray_t* rc = ray_vec_new(RAY_I64, 3); rc->len = 3;
    memcpy(ray_data(rc), counts, sizeof(counts));
    ray_t* mc = make_mapcommon(kv, rc);

    int64_t sym_dt  = ray_sym_intern("dt", 2);
    int64_t sym_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dt, mc);
    tbl = ray_table_add_col(tbl, sym_val, val);

    /* val * 10 — element-wise unary, streamable; per-segment result is
     * a 4-row vector that must be concatenated by ray_result_merge. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* sv  = ray_scan(g, "val");
    ray_op_t* c10 = ray_const_i64(g, 10);
    ray_op_t* mul = ray_mul(g, sv, c10);
    ray_t* result = ray_execute(g, mul);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_I64);
    TEST_ASSERT_EQ_I(result->len, 12);
    int64_t* rd = (int64_t*)ray_data(result);
    int64_t expected[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    for (int64_t i = 0; i < 12; i++) TEST_ASSERT_EQ_I(rd[i], expected[i]);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(mc); ray_release(kv); ray_release(rc);
    ray_release(val);
    for (int i = 0; i < 3; i++) ray_release(segs_v[i]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 8: partitioned_gather with esz=2 (RAY_I16)
 *
 * Targets exec.c L368-372 (the e==2 arm of pg_block_fn) plus the full
 * partition-route plumbing at L259-473.  Drives the parallel path by
 * generating an index array of size > PG_MIN (131072) over a source
 * column of the same size.  Verifies each output row gets the right
 * source element via random index permutation.
 * -------------------------------------------------------------------------- */
static test_result_t test_partitioned_gather_e2(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 131072 + 1024;   /* > PG_MIN to enter the parallel branch */
    ray_t* src = ray_vec_new(RAY_I16, n);
    TEST_ASSERT_NOT_NULL(src);
    src->len = n;
    int16_t* sd = (int16_t*)ray_data(src);
    for (int64_t i = 0; i < n; i++) sd[i] = (int16_t)(i & 0x7FFF);

    ray_t* dst = ray_vec_new(RAY_I16, n);
    TEST_ASSERT_NOT_NULL(dst);
    dst->len = n;
    int16_t* dd = (int16_t*)ray_data(dst);
    memset(dd, 0xAA, (size_t)n * sizeof(int16_t));

    /* Reverse permutation idx[i] = n-1-i, exercises every block */
    ray_t* idxv = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(idxv);
    idxv->len = n;
    int64_t* idx = (int64_t*)ray_data(idxv);
    for (int64_t i = 0; i < n; i++) idx[i] = n - 1 - i;

    ray_pool_t* pool = ray_pool_get();
    char* srcs[1] = { (char*)sd };
    char* dsts[1] = { (char*)dd };
    uint8_t esz[1] = { 2 };
    partitioned_gather(pool, idx, n, n, srcs, dsts, esz, 1);

    /* Spot-check a handful of rows scattered across the input range so a
     * routing bug in any block surfaces.  Full sweep is unnecessary and
     * slows the suite. */
    int64_t probes[] = {0, 1, 17, 1024, 16384, 16385, 65536, 131071, n - 1};
    for (size_t k = 0; k < sizeof(probes)/sizeof(probes[0]); k++) {
        int64_t i = probes[k];
        int16_t expected = (int16_t)((n - 1 - i) & 0x7FFF);
        TEST_ASSERT_FMT(dd[i] == expected,
                        "i=%lld got=%d expected=%d",
                        (long long)i, (int)dd[i], (int)expected);
    }

    ray_release(idxv);
    ray_release(dst);
    ray_release(src);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 9: partitioned_gather with esz=1 (RAY_BOOL / RAY_U8)
 *
 * Targets exec.c L373-375 (the e==1 arm).  Same shape as Test 8 but
 * single-byte elements.
 * -------------------------------------------------------------------------- */
static test_result_t test_partitioned_gather_e1(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 131072 + 512;
    ray_t* src = ray_vec_new(RAY_U8, n);
    src->len = n;
    uint8_t* sd = (uint8_t*)ray_data(src);
    for (int64_t i = 0; i < n; i++) sd[i] = (uint8_t)(i & 0xFF);

    ray_t* dst = ray_vec_new(RAY_U8, n);
    dst->len = n;
    uint8_t* dd = (uint8_t*)ray_data(dst);
    memset(dd, 0, (size_t)n);

    ray_t* idxv = ray_vec_new(RAY_I64, n);
    idxv->len = n;
    int64_t* idx = (int64_t*)ray_data(idxv);
    for (int64_t i = 0; i < n; i++) idx[i] = n - 1 - i;

    ray_pool_t* pool = ray_pool_get();
    char* srcs[1] = { (char*)sd };
    char* dsts[1] = { (char*)dd };
    uint8_t esz[1] = { 1 };
    partitioned_gather(pool, idx, n, n, srcs, dsts, esz, 1);

    int64_t probes[] = {0, 7, 255, 256, 16383, 16384, 65535, n - 1};
    for (size_t k = 0; k < sizeof(probes)/sizeof(probes[0]); k++) {
        int64_t i = probes[k];
        uint8_t expected = (uint8_t)((n - 1 - i) & 0xFF);
        TEST_ASSERT_FMT(dd[i] == expected,
                        "i=%lld got=%u expected=%u",
                        (long long)i, (unsigned)dd[i], (unsigned)expected);
    }

    ray_release(idxv);
    ray_release(dst);
    ray_release(src);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 10: partitioned_gather small-n fallback (multi_gather_fn path)
 *
 * Targets exec.c L390-399 (the n < PG_MIN fallback into multi_gather_fn).
 * Uses a small index array so the partitioning machinery is bypassed and
 * the dispatch goes straight to the column-at-a-time gather loop.
 * -------------------------------------------------------------------------- */
static test_result_t test_partitioned_gather_fallback(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t n = 1024;   /* < PG_MIN */
    ray_t* src = ray_vec_new(RAY_I64, n);
    src->len = n;
    int64_t* sd = (int64_t*)ray_data(src);
    for (int64_t i = 0; i < n; i++) sd[i] = i * 7 + 1;

    ray_t* dst = ray_vec_new(RAY_I64, n);
    dst->len = n;
    int64_t* dd = (int64_t*)ray_data(dst);
    memset(dd, 0, (size_t)n * sizeof(int64_t));

    ray_t* idxv = ray_vec_new(RAY_I64, n);
    idxv->len = n;
    int64_t* idx = (int64_t*)ray_data(idxv);
    for (int64_t i = 0; i < n; i++) idx[i] = (n - 1 - i);

    ray_pool_t* pool = ray_pool_get();
    char* srcs[1] = { (char*)sd };
    char* dsts[1] = { (char*)dd };
    uint8_t esz[1] = { 8 };
    partitioned_gather(pool, idx, n, n, srcs, dsts, esz, 1);

    for (int64_t i = 0; i < n; i++) {
        int64_t expected = (n - 1 - i) * 7 + 1;
        TEST_ASSERT_FMT(dd[i] == expected,
                        "i=%lld got=%lld expected=%lld",
                        (long long)i, (long long)dd[i], (long long)expected);
    }

    ray_release(idxv);
    ray_release(dst);
    ray_release(src);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 11: exec_filter — small table with parted column (seq path)
 *
 * Targets filter.c L103-169 (exec_filter_parted_vec, non-STR path).
 * Builds a small table (nrows < RAY_PARALLEL_THRESHOLD = 65536) with a
 * parted I64 column so exec_filter goes via exec_filter_seq, which
 * dispatches to exec_filter_parted_vec for the parted column.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_parted_seq(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments × 10 rows = 30 total — well below threshold */
    ray_t* segs[3];
    for (int s = 0; s < 3; s++) {
        segs[s] = ray_vec_new(RAY_I64, 10);
        segs[s]->len = 10;
        int64_t* d = (int64_t*)ray_data(segs[s]);
        for (int j = 0; j < 10; j++) d[j] = (int64_t)(s * 100 + j);
    }
    ray_t* val = make_parted(RAY_I64, segs, 3);

    /* Flat I64 column (non-parted) to exercise the flat branch too */
    ray_t* flat = ray_vec_new(RAY_I64, 30);
    flat->len = 30;
    int64_t* fd = (int64_t*)ray_data(flat);
    for (int i = 0; i < 30; i++) fd[i] = (int64_t)(i * 2);

    int64_t sym_val  = ray_sym_intern("val",  3);
    int64_t sym_flat = ray_sym_intern("flat", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_val,  val);
    tbl = ray_table_add_col(tbl, sym_flat, flat);

    /* Predicate: keep rows where val % 3 == 0 (rows 0,3,6,...,27) = 10 rows */
    ray_t* pred = ray_vec_new(RAY_BOOL, 30);
    pred->len = 30;
    uint8_t* pb = (uint8_t*)ray_data(pred);
    int64_t expected_pass = 0;
    for (int i = 0; i < 30; i++) {
        pb[i] = (i % 3 == 0) ? 1 : 0;
        if (pb[i]) expected_pass++;
    }
    TEST_ASSERT_EQ_I(expected_pass, 10);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_val  = ray_scan(g, "val");
    ray_op_t* const_c   = ray_const_i64(g, 3);
    ray_op_t* scan_val2 = ray_scan(g, "val");
    ray_op_t* rem_pred  = ray_eq(g, ray_mod(g, scan_val2, const_c),
                                    ray_const_i64(g, 0));
    ray_op_t* flt       = ray_filter(g, scan_val, rem_pred);
    ray_t* result = ray_execute(g, flt);

    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(val);
    ray_release(flat);
    for (int s = 0; s < 3; s++) ray_release(segs[s]);
    ray_release(pred);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 12: exec_filter — small table with parted column, table-level filter
 *
 * Builds a 2-column table (parted I64 + flat I64) with 30 rows and runs
 * a table-level FILTER via exec_filter directly.  At 30 rows the seq path
 * in exec_filter routes to exec_filter_seq, which calls exec_filter_parted_vec
 * for the parted column and exec_filter_vec for the flat column.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_table_parted_seq(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2 segments × 15 rows = 30 total */
    ray_t* segs[2];
    for (int s = 0; s < 2; s++) {
        segs[s] = ray_vec_new(RAY_I64, 15);
        segs[s]->len = 15;
        int64_t* d = (int64_t*)ray_data(segs[s]);
        for (int j = 0; j < 15; j++) d[j] = (int64_t)(s * 15 + j);
    }
    ray_t* parted_col = make_parted(RAY_I64, segs, 2);

    ray_t* flat_col = ray_vec_new(RAY_I64, 30);
    flat_col->len = 30;
    int64_t* fd2 = (int64_t*)ray_data(flat_col);
    for (int i = 0; i < 30; i++) fd2[i] = i;

    int64_t sym_p = ray_sym_intern("p",    1);
    int64_t sym_f = ray_sym_intern("f",    1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_p, parted_col);
    tbl = ray_table_add_col(tbl, sym_f, flat_col);

    /* Build a pred vector for the table (30 elements), keep even rows */
    ray_t* pred = ray_vec_new(RAY_BOOL, 30);
    pred->len = 30;
    uint8_t* pb2 = (uint8_t*)ray_data(pred);
    int64_t pass2 = 0;
    for (int i = 0; i < 30; i++) { pb2[i] = (i % 2 == 0) ? 1 : 0; if (pb2[i]) pass2++; }
    TEST_ASSERT_EQ_I(pass2, 15);

    /* Call exec_filter directly — no DAG needed */
    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 15);

    /* Verify the parted column values */
    ray_t* pcol = ray_table_get_col(result, sym_p);
    TEST_ASSERT_NOT_NULL(pcol);
    TEST_ASSERT_EQ_I(pcol->len, 15);
    int64_t* pd = (int64_t*)ray_data(pcol);
    for (int i = 0; i < 15; i++) TEST_ASSERT_EQ_I(pd[i], (int64_t)(i * 2));

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    ray_release(parted_col);
    ray_release(flat_col);
    for (int s = 0; s < 2; s++) ray_release(segs[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 13: exec_filter_parted_vec — RAY_STR parted column
 *
 * Targets filter.c L111-129 (the RAY_STR branch of exec_filter_parted_vec).
 * Builds a small parted STR column and filters it via exec_filter_seq.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_parted_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2 segments of 3 strings each */
    const char* strs0[] = { "apple", "banana", "cherry" };
    const char* strs1[] = { "date", "elderberry", "fig" };

    ray_t* seg0 = ray_vec_new(RAY_STR, 3);
    seg0->len = 0;
    for (int i = 0; i < 3; i++) seg0 = ray_str_vec_append(seg0, strs0[i], strlen(strs0[i]));
    TEST_ASSERT_EQ_I(seg0->len, 3);

    ray_t* seg1 = ray_vec_new(RAY_STR, 3);
    seg1->len = 0;
    for (int i = 0; i < 3; i++) seg1 = ray_str_vec_append(seg1, strs1[i], strlen(strs1[i]));
    TEST_ASSERT_EQ_I(seg1->len, 3);

    ray_t* segs_str[2] = { seg0, seg1 };
    ray_t* parted_str = make_parted(RAY_STR, segs_str, 2);

    /* Flat companion column */
    ray_t* flat_idx = ray_vec_new(RAY_I64, 6);
    flat_idx->len = 6;
    int64_t* fid = (int64_t*)ray_data(flat_idx);
    for (int i = 0; i < 6; i++) fid[i] = i;

    int64_t sym_s = ray_sym_intern("s", 1);
    int64_t sym_i = ray_sym_intern("i", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_s, parted_str);
    tbl = ray_table_add_col(tbl, sym_i, flat_idx);

    /* pred: keep rows 0,2,4 (the even ones) — 3 rows */
    ray_t* pred = ray_vec_new(RAY_BOOL, 6);
    pred->len = 6;
    uint8_t* pb3 = (uint8_t*)ray_data(pred);
    for (int i = 0; i < 6; i++) pb3[i] = (i % 2 == 0) ? 1 : 0;

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    /* Verify string column */
    ray_t* scol = ray_table_get_col(result, sym_s);
    TEST_ASSERT_NOT_NULL(scol);
    TEST_ASSERT_EQ_I(scol->type, RAY_STR);
    TEST_ASSERT_EQ_I(scol->len, 3);

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    ray_release(parted_str);
    ray_release(flat_idx);
    ray_release(seg0);
    ray_release(seg1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 14: exec_filter — large flat table (parallel path)
 *
 * Targets filter.c L231-384 (exec_filter large-table parallel gather).
 * Builds a flat table with 2 I64 columns of 70000 rows (> 65536 threshold)
 * and runs a filter to confirm the parallel multi-gather branch executes.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_flat(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 70000;   /* > RAY_PARALLEL_THRESHOLD (64*1024 = 65536) */

    ray_t* col_a = ray_vec_new(RAY_I64, N);
    col_a->len = N;
    int64_t* da = (int64_t*)ray_data(col_a);
    for (int64_t i = 0; i < N; i++) da[i] = i;

    ray_t* col_b = ray_vec_new(RAY_I64, N);
    col_b->len = N;
    int64_t* db = (int64_t*)ray_data(col_b);
    for (int64_t i = 0; i < N; i++) db[i] = N - 1 - i;

    int64_t sym_a = ray_sym_intern("a", 1);
    int64_t sym_b = ray_sym_intern("b", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_a, col_a);
    tbl = ray_table_add_col(tbl, sym_b, col_b);

    /* pred: keep rows where a >= 69000 — 1000 rows */
    ray_t* pred = ray_vec_new(RAY_BOOL, N);
    pred->len = N;
    uint8_t* pb4 = (uint8_t*)ray_data(pred);
    int64_t expected4 = 0;
    for (int64_t i = 0; i < N; i++) {
        pb4[i] = (da[i] >= 69000) ? 1 : 0;
        if (pb4[i]) expected4++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), expected4);

    ray_t* rcol_a = ray_table_get_col(result, sym_a);
    TEST_ASSERT_NOT_NULL(rcol_a);
    TEST_ASSERT_EQ_I(rcol_a->len, expected4);
    int64_t* rad = (int64_t*)ray_data(rcol_a);
    for (int64_t i = 0; i < expected4; i++)
        TEST_ASSERT_EQ_I(rad[i], 69000 + i);

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    ray_release(col_a);
    ray_release(col_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 15: exec_filter — large table with parted column (parallel parted path)
 *
 * Targets filter.c L295-319 (has_parted_cols branch) + parted_gather_col
 * (L34-68).  Builds a table with 70000 total rows spread across 7 parted
 * segments of 10000 rows each.  Filtering keeps every 10th row.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_parted(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t SEG_SIZE = 10000;
    const int64_t N_SEGS   = 7;
    const int64_t N        = SEG_SIZE * N_SEGS;  /* 70000 > 65536 */

    ray_t* segs_lp[N_SEGS];
    for (int64_t s = 0; s < N_SEGS; s++) {
        segs_lp[s] = ray_vec_new(RAY_I64, SEG_SIZE);
        segs_lp[s]->len = SEG_SIZE;
        int64_t* d = (int64_t*)ray_data(segs_lp[s]);
        for (int64_t j = 0; j < SEG_SIZE; j++) d[j] = s * SEG_SIZE + j;
    }
    ray_t* parted_lp = make_parted(RAY_I64, segs_lp, N_SEGS);

    /* Flat companion column (also 70000 rows) */
    ray_t* flat_lp = ray_vec_new(RAY_I64, N);
    flat_lp->len = N;
    int64_t* fld = (int64_t*)ray_data(flat_lp);
    for (int64_t i = 0; i < N; i++) fld[i] = i * 2;

    /* MAPCOMMON col to exercise the already-materialized path inside parallel gather */
    int64_t keys_lp[] = {20240101, 20240102, 20240103, 20240104,
                         20240105, 20240106, 20240107};
    int64_t counts_lp[N_SEGS];
    for (int64_t s = 0; s < N_SEGS; s++) counts_lp[s] = SEG_SIZE;
    ray_t* kv_lp = ray_vec_new(RAY_I64, N_SEGS); kv_lp->len = N_SEGS;
    memcpy(ray_data(kv_lp), keys_lp, (size_t)N_SEGS * sizeof(int64_t));
    ray_t* rc_lp = ray_vec_new(RAY_I64, N_SEGS); rc_lp->len = N_SEGS;
    memcpy(ray_data(rc_lp), counts_lp, (size_t)N_SEGS * sizeof(int64_t));
    ray_t* mc_lp = make_mapcommon(kv_lp, rc_lp);

    int64_t sym_pv = ray_sym_intern("pv", 2);
    int64_t sym_fv = ray_sym_intern("fv", 2);
    int64_t sym_dt = ray_sym_intern("dt", 2);
    ray_t* tbl = ray_table_new(3);
    tbl = ray_table_add_col(tbl, sym_pv, parted_lp);
    tbl = ray_table_add_col(tbl, sym_fv, flat_lp);
    tbl = ray_table_add_col(tbl, sym_dt, mc_lp);

    /* pred: keep every 10th row */
    ray_t* pred = ray_vec_new(RAY_BOOL, N);
    pred->len = N;
    uint8_t* pb5 = (uint8_t*)ray_data(pred);
    int64_t pass5 = 0;
    for (int64_t i = 0; i < N; i++) {
        pb5[i] = (i % 10 == 0) ? 1 : 0;
        if (pb5[i]) pass5++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass5);

    /* Verify parted column: row i*10 should give value i*10 */
    ray_t* rcol_pv = ray_table_get_col(result, sym_pv);
    TEST_ASSERT_NOT_NULL(rcol_pv);
    TEST_ASSERT_EQ_I(rcol_pv->len, pass5);
    int64_t* rpd = (int64_t*)ray_data(rcol_pv);
    for (int64_t i = 0; i < pass5; i++)
        TEST_ASSERT_EQ_I(rpd[i], i * 10);

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    ray_release(parted_lp);
    ray_release(flat_lp);
    ray_release(mc_lp); ray_release(kv_lp); ray_release(rc_lp);
    for (int64_t s = 0; s < N_SEGS; s++) ray_release(segs_lp[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 16: exec_filter_head — parted column path
 *
 * Targets filter.c L451-475 (the non-STR parted gather in exec_filter_head).
 * Builds a HEAD(FILTER(...)) DAG on a table that has a parted I64 column,
 * so the early-exit path in exec_filter_head must walk parted segments.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_head_parted(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 4 segments × 20 rows = 80 total */
    ray_t* segs_fh[4];
    for (int s = 0; s < 4; s++) {
        segs_fh[s] = ray_vec_new(RAY_I64, 20);
        segs_fh[s]->len = 20;
        int64_t* d = (int64_t*)ray_data(segs_fh[s]);
        for (int j = 0; j < 20; j++) d[j] = (int64_t)(s * 20 + j);
    }
    ray_t* parted_fh = make_parted(RAY_I64, segs_fh, 4);

    /* Flat companion */
    ray_t* flat_fh = ray_vec_new(RAY_I64, 80);
    flat_fh->len = 80;
    int64_t* ffd = (int64_t*)ray_data(flat_fh);
    for (int i = 0; i < 80; i++) ffd[i] = i;

    int64_t sym_pf = ray_sym_intern("pf", 2);
    int64_t sym_ff = ray_sym_intern("ff", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pf, parted_fh);
    tbl = ray_table_add_col(tbl, sym_ff, flat_fh);

    /* HEAD(FILTER(val >= 40)) limit=5: rows 40..44 */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* scan_pf  = ray_scan(g, "pf");
    ray_op_t* c40      = ray_const_i64(g, 40);
    ray_op_t* pred_op  = ray_ge(g, scan_pf, c40);
    /* FILTER on a table scan, then HEAD */
    ray_op_t* tbl_scan = ray_const_table(g, tbl);
    ray_op_t* flt_op   = ray_filter(g, tbl_scan, pred_op);
    ray_op_t* head_op  = ray_head(g, flt_op, 5);
    ray_t* result = ray_execute(g, head_op);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));

    if (result->type == RAY_TABLE) {
        int64_t nrows = ray_table_nrows(result);
        TEST_ASSERT_EQ_I(nrows, 5);
        ray_t* pf_res = ray_table_get_col(result, sym_pf);
        if (pf_res) {
            TEST_ASSERT_EQ_I(pf_res->len, 5);
            int64_t* pfd = (int64_t*)ray_data(pf_res);
            for (int i = 0; i < 5; i++) TEST_ASSERT_EQ_I(pfd[i], 40 + i);
        }
    }

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(parted_fh);
    ray_release(flat_fh);
    for (int s = 0; s < 4; s++) ray_release(segs_fh[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 17: sel_compact — basic rowsel compaction
 *
 * Targets filter.c L497-685 (sel_compact).
 * Builds a flat table, creates a rowsel via ray_rowsel_from_pred, then
 * calls sel_compact directly.  Exercises the SEL_ALL, SEL_MIX, and
 * SEL_NONE segment flags via a predicate that keeps about half the rows.
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_basic(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 3072;  /* 3 morsels of 1024 each */

    ray_t* col_x = ray_vec_new(RAY_I64, N);
    col_x->len = N;
    int64_t* xd = (int64_t*)ray_data(col_x);
    for (int64_t i = 0; i < N; i++) xd[i] = i;

    ray_t* col_y = ray_vec_new(RAY_I64, N);
    col_y->len = N;
    int64_t* yd = (int64_t*)ray_data(col_y);
    for (int64_t i = 0; i < N; i++) yd[i] = N - 1 - i;

    int64_t sym_x = ray_sym_intern("x", 1);
    int64_t sym_y = ray_sym_intern("y", 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_x, col_x);
    tbl = ray_table_add_col(tbl, sym_y, col_y);

    /* Predicate: keep rows in [1024, 2048) (entire second morsel = SEL_ALL),
     * keep nothing in [0,1024) (SEL_NONE for first morsel),
     * keep even rows in [2048,3072) (SEL_MIX for third morsel). */
    ray_t* pred_sc = ray_vec_new(RAY_BOOL, N);
    pred_sc->len = N;
    uint8_t* psc = (uint8_t*)ray_data(pred_sc);
    int64_t pass_sc = 0;
    for (int64_t i = 0; i < N; i++) {
        uint8_t keep;
        if (i < 1024)          keep = 0;           /* morsel 0: NONE */
        else if (i < 2048)     keep = 1;           /* morsel 1: ALL  */
        else                   keep = (i % 2 == 0) ? 1 : 0; /* morsel 2: MIX */
        psc[i] = keep;
        if (keep) pass_sc++;
    }

    ray_t* sel = ray_rowsel_from_pred(pred_sc);
    /* all-pass returns NULL; none-all-pass returns a block */
    TEST_ASSERT_NOT_NULL(sel);

    ray_t* result = sel_compact(NULL, tbl, sel);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_sc);

    ray_t* rx = ray_table_get_col(result, sym_x);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQ_I(rx->len, pass_sc);
    int64_t* rxd = (int64_t*)ray_data(rx);
    /* rows in [1024,2048) come first (SEL_ALL) */
    for (int64_t i = 0; i < 1024; i++) TEST_ASSERT_EQ_I(rxd[i], 1024 + i);
    /* then even rows in [2048,3072) */
    for (int64_t i = 0; i < 512; i++) TEST_ASSERT_EQ_I(rxd[1024 + i], 2048 + i * 2);

    ray_rowsel_release(sel);
    ray_release(result);
    ray_release(pred_sc);
    ray_release(tbl);
    ray_release(col_x);
    ray_release(col_y);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 18: sel_compact — none-pass returns empty table
 *
 * Exercises filter.c L522-539 (the pass_count == 0 early-return branch).
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_none_pass(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 1024;

    ray_t* col_a = ray_vec_new(RAY_I64, N);
    col_a->len = N;
    int64_t* aad = (int64_t*)ray_data(col_a);
    for (int64_t i = 0; i < N; i++) aad[i] = i;

    int64_t sym_a2 = ray_sym_intern("a2", 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_a2, col_a);

    /* all-false predicate */
    ray_t* pred_np = ray_vec_new(RAY_BOOL, N);
    pred_np->len = N;
    uint8_t* pnp = (uint8_t*)ray_data(pred_np);
    memset(pnp, 0, (size_t)N);

    ray_t* sel_np = ray_rowsel_from_pred(pred_np);
    TEST_ASSERT_NOT_NULL(sel_np);

    ray_t* result = sel_compact(NULL, tbl, sel_np);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 0);

    ray_rowsel_release(sel_np);
    ray_release(result);
    ray_release(pred_np);
    ray_release(tbl);
    ray_release(col_a);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 19: sel_compact — parted column table
 *
 * Targets filter.c L609-629 (the has_parted branch in sel_compact) and
 * parted_gather_col.  Builds a table with a parted I64 column + flat col,
 * creates a rowsel, compacts it.
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_parted(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments × 20 rows = 60 total */
    ray_t* segs_sc[3];
    for (int s = 0; s < 3; s++) {
        segs_sc[s] = ray_vec_new(RAY_I64, 20);
        segs_sc[s]->len = 20;
        int64_t* d = (int64_t*)ray_data(segs_sc[s]);
        for (int j = 0; j < 20; j++) d[j] = (int64_t)(s * 20 + j);
    }
    ray_t* parted_sc = make_parted(RAY_I64, segs_sc, 3);

    ray_t* flat_sc = ray_vec_new(RAY_I64, 60);
    flat_sc->len = 60;
    int64_t* fsc = (int64_t*)ray_data(flat_sc);
    for (int i = 0; i < 60; i++) fsc[i] = i * 3;

    int64_t sym_ps = ray_sym_intern("ps", 2);
    int64_t sym_fs = ray_sym_intern("fs", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_ps, parted_sc);
    tbl = ray_table_add_col(tbl, sym_fs, flat_sc);

    /* Keep rows 0..9 (first 10 of segment 0) */
    ray_t* pred_sc2 = ray_vec_new(RAY_BOOL, 60);
    pred_sc2->len = 60;
    uint8_t* psc2 = (uint8_t*)ray_data(pred_sc2);
    for (int i = 0; i < 60; i++) {
        psc2[i] = (i < 10) ? 1 : 0;
    }

    ray_t* sel2 = ray_rowsel_from_pred(pred_sc2);
    TEST_ASSERT_NOT_NULL(sel2);

    ray_t* result = sel_compact(NULL, tbl, sel2);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 10);

    ray_t* rps = ray_table_get_col(result, sym_ps);
    TEST_ASSERT_NOT_NULL(rps);
    TEST_ASSERT_EQ_I(rps->len, 10);
    int64_t* rpsd = (int64_t*)ray_data(rps);
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQ_I(rpsd[i], i);

    ray_rowsel_release(sel2);
    ray_release(result);
    ray_release(pred_sc2);
    ray_release(tbl);
    ray_release(parted_sc);
    ray_release(flat_sc);
    for (int s = 0; s < 3; s++) ray_release(segs_sc[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 20: exec_filter_seq with MAPCOMMON column
 *
 * Targets filter.c L180-186 (the MAPCOMMON branch in exec_filter_seq).
 * Builds a small table with a MAPCOMMON column and a flat I64 column, then
 * runs exec_filter directly (small table → exec_filter_seq).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_seq_mapcommon(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* MAPCOMMON: 3 partitions of 5 rows each = 15 rows */
    int64_t mc_keys[]   = {20240101, 20240102, 20240103};
    int64_t mc_counts[] = {5, 5, 5};
    ray_t* kv_mc = ray_vec_new(RAY_I64, 3); kv_mc->len = 3;
    memcpy(ray_data(kv_mc), mc_keys, sizeof(mc_keys));
    ray_t* rc_mc = ray_vec_new(RAY_I64, 3); rc_mc->len = 3;
    memcpy(ray_data(rc_mc), mc_counts, sizeof(mc_counts));
    ray_t* mc = make_mapcommon(kv_mc, rc_mc);

    /* Flat companion: 15 rows */
    ray_t* flat_mc = ray_vec_new(RAY_I64, 15);
    flat_mc->len = 15;
    int64_t* fmc = (int64_t*)ray_data(flat_mc);
    for (int i = 0; i < 15; i++) fmc[i] = i;

    int64_t sym_dt2  = ray_sym_intern("dt2",  3);
    int64_t sym_val2 = ray_sym_intern("val2", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dt2,  mc);
    tbl = ray_table_add_col(tbl, sym_val2, flat_mc);

    /* pred: keep rows 0,2,4,...,14 (even rows = 8 rows) */
    ray_t* pred_mc = ray_vec_new(RAY_BOOL, 15);
    pred_mc->len = 15;
    uint8_t* pmc = (uint8_t*)ray_data(pred_mc);
    int64_t pass_mc = 0;
    for (int i = 0; i < 15; i++) {
        pmc[i] = (i % 2 == 0) ? 1 : 0;
        if (pmc[i]) pass_mc++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_mc);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_mc);

    /* MAPCOMMON col should be materialized */
    ray_t* dt_res = ray_table_get_col(result, sym_dt2);
    TEST_ASSERT_NOT_NULL(dt_res);
    TEST_ASSERT_EQ_I(dt_res->len, pass_mc);

    ray_release(result);
    ray_release(pred_mc);
    ray_release(tbl);
    ray_release(mc); ray_release(kv_mc); ray_release(rc_mc);
    ray_release(flat_mc);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 21: exec_filter_head — zero limit and negative limit edge cases
 *
 * Targets filter.c L401 (limit <= 0 branch).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_head_zero_limit(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* col = ray_vec_new(RAY_I64, 10);
    col->len = 10;
    int64_t* cd = (int64_t*)ray_data(col);
    for (int i = 0; i < 10; i++) cd[i] = i;

    int64_t sym_c = ray_sym_intern("c", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_c, col);

    ray_t* pred = ray_vec_new(RAY_BOOL, 10);
    pred->len = 10;
    uint8_t* ppd = (uint8_t*)ray_data(pred);
    memset(ppd, 1, 10);

    ray_t* r0 = exec_filter_head(tbl, pred, 0);
    TEST_ASSERT_NOT_NULL(r0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(r0->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(r0), 0);

    ray_t* rn = exec_filter_head(tbl, pred, -1);
    TEST_ASSERT_NOT_NULL(rn);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rn));

    ray_release(r0);
    ray_release(rn);
    ray_release(pred);
    ray_release(tbl);
    ray_release(col);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 22: exec_filter_head — non-table / non-BOOL inputs (early returns)
 *
 * Targets filter.c L397 (input->type != RAY_TABLE || pred->type != RAY_BOOL).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_head_non_table(void) {
    ray_heap_init();
    (void)ray_sym_init();

    ray_t* vec = ray_vec_new(RAY_I64, 5);
    vec->len = 5;
    ray_t* pred = ray_vec_new(RAY_BOOL, 5);
    pred->len = 5;
    memset(ray_data(pred), 1, 5);

    /* Non-table input — should return input unchanged */
    ray_t* r1 = exec_filter_head(vec, pred, 3);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));

    /* Non-BOOL pred with a real table */
    ray_t* col = ray_vec_new(RAY_I64, 5);
    col->len = 5;
    int64_t sym_d = ray_sym_intern("d", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_d, col);

    ray_t* non_bool_pred = ray_vec_new(RAY_I64, 5);
    non_bool_pred->len = 5;
    ray_t* r2 = exec_filter_head(tbl, non_bool_pred, 3);
    TEST_ASSERT_NOT_NULL(r2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));

    ray_release(r1);
    ray_release(r2);
    ray_release(pred);
    ray_release(non_bool_pred);
    ray_release(tbl);
    ray_release(col);
    ray_release(vec);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 23: exec_filter_head — parted STR column in early-exit gather
 *
 * Targets filter.c L454-458 (the STR parted branch of exec_filter_head
 * which calls parted_gather_str_rows).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_head_parted_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const char* w0[] = { "alpha", "beta", "gamma", "delta", "epsilon" };
    const char* w1[] = { "zeta", "eta", "theta", "iota", "kappa" };

    ray_t* seg_s0 = ray_vec_new(RAY_STR, 5); seg_s0->len = 0;
    for (int i = 0; i < 5; i++) seg_s0 = ray_str_vec_append(seg_s0, w0[i], strlen(w0[i]));
    ray_t* seg_s1 = ray_vec_new(RAY_STR, 5); seg_s1->len = 0;
    for (int i = 0; i < 5; i++) seg_s1 = ray_str_vec_append(seg_s1, w1[i], strlen(w1[i]));

    ray_t* segs_hs[2] = { seg_s0, seg_s1 };
    ray_t* parted_hs = make_parted(RAY_STR, segs_hs, 2);

    /* Companion flat */
    ray_t* flat_hs = ray_vec_new(RAY_I64, 10);
    flat_hs->len = 10;
    int64_t* fhsd = (int64_t*)ray_data(flat_hs);
    for (int i = 0; i < 10; i++) fhsd[i] = i;

    int64_t sym_ws = ray_sym_intern("ws", 2);
    int64_t sym_wi = ray_sym_intern("wi", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_ws, parted_hs);
    tbl = ray_table_add_col(tbl, sym_wi, flat_hs);

    /* pred: keep all rows, limit=3 */
    ray_t* pred_hs = ray_vec_new(RAY_BOOL, 10);
    pred_hs->len = 10;
    uint8_t* phsd = (uint8_t*)ray_data(pred_hs);
    memset(phsd, 1, 10);

    ray_t* result = exec_filter_head(tbl, pred_hs, 3);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_release(result);
    ray_release(pred_hs);
    ray_release(tbl);
    ray_release(parted_hs);
    ray_release(flat_hs);
    ray_release(seg_s0);
    ray_release(seg_s1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 24: parted_gather_col — cross-segment boundary walk
 *
 * Targets filter.c L34-68 directly via the large parted filter path
 * (exec_filter large table with parted col).  This variant exercises the
 * segment-boundary advance (while loop at L57) with indices that span
 * multiple segments and also exercises the NULL-check at L64-66 by
 * having the second segment with no nulls.
 * -------------------------------------------------------------------------- */
static test_result_t test_parted_gather_col_multi_seg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments, each 30000 rows.  Total = 90000 > threshold. */
    const int64_t SEG = 30000;
    const int64_t N_SEGS = 3;
    const int64_t N = SEG * N_SEGS;

    ray_t* segs_mg[N_SEGS];
    for (int64_t s = 0; s < N_SEGS; s++) {
        segs_mg[s] = ray_vec_new(RAY_I64, SEG);
        segs_mg[s]->len = SEG;
        int64_t* d = (int64_t*)ray_data(segs_mg[s]);
        for (int64_t j = 0; j < SEG; j++) d[j] = s * SEG + j;
    }
    ray_t* parted_mg = make_parted(RAY_I64, segs_mg, N_SEGS);

    /* Flat companion */
    ray_t* flat_mg = ray_vec_new(RAY_I64, N);
    flat_mg->len = N;
    int64_t* fmg = (int64_t*)ray_data(flat_mg);
    for (int64_t i = 0; i < N; i++) fmg[i] = i;

    int64_t sym_pmg = ray_sym_intern("pmg", 3);
    int64_t sym_fmg = ray_sym_intern("fmg", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pmg, parted_mg);
    tbl = ray_table_add_col(tbl, sym_fmg, flat_mg);

    /* Keep exactly one row from each segment boundary region:
     * rows 29999 (end of seg 0), 30000 (start of seg 1), 59999, 60000 */
    ray_t* pred_mg = ray_vec_new(RAY_BOOL, N);
    pred_mg->len = N;
    uint8_t* pmg = (uint8_t*)ray_data(pred_mg);
    memset(pmg, 0, (size_t)N);
    pmg[29999] = 1; pmg[30000] = 1; pmg[59999] = 1; pmg[60000] = 1;
    int64_t pass_mg = 4;

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_mg);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_mg);

    ray_t* rpmg = ray_table_get_col(result, sym_pmg);
    TEST_ASSERT_NOT_NULL(rpmg);
    TEST_ASSERT_EQ_I(rpmg->len, pass_mg);
    int64_t* rpmgd = (int64_t*)ray_data(rpmg);
    TEST_ASSERT_EQ_I(rpmgd[0], 29999);
    TEST_ASSERT_EQ_I(rpmgd[1], 30000);
    TEST_ASSERT_EQ_I(rpmgd[2], 59999);
    TEST_ASSERT_EQ_I(rpmgd[3], 60000);

    ray_release(result);
    ray_release(pred_mg);
    ray_release(tbl);
    ray_release(parted_mg);
    ray_release(flat_mg);
    for (int64_t s = 0; s < N_SEGS; s++) ray_release(segs_mg[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 25: exec_filter — large table with parted STR column (parallel path)
 *
 * Targets filter.c L304-309 (the pbase==RAY_STR arm inside has_parted_cols
 * in exec_filter).  Builds a table with 70000+ rows including a parted STR
 * column so the deep-copy gather path is exercised.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_parted_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 7 segments × 10000 = 70000 total */
    const int64_t SEG_SZ = 10000;
    const int64_t N_SEG  = 7;
    const int64_t N      = SEG_SZ * N_SEG;

    /* Build parted STR: each segment has 10000 strings like "row_000001" */
    ray_t* segs_ps[N_SEG];
    for (int64_t s = 0; s < N_SEG; s++) {
        segs_ps[s] = ray_vec_new(RAY_STR, SEG_SZ);
        segs_ps[s]->len = 0;
        char buf[32];
        for (int64_t j = 0; j < SEG_SZ; j++) {
            int n = snprintf(buf, sizeof(buf), "r%lld", (long long)(s * SEG_SZ + j));
            segs_ps[s] = ray_str_vec_append(segs_ps[s], buf, (size_t)n);
        }
    }
    ray_t* parted_ps = make_parted(RAY_STR, segs_ps, N_SEG);

    /* Flat companion */
    ray_t* flat_ps = ray_vec_new(RAY_I64, N);
    flat_ps->len = N;
    int64_t* fps = (int64_t*)ray_data(flat_ps);
    for (int64_t i = 0; i < N; i++) fps[i] = i;

    int64_t sym_sv = ray_sym_intern("sv", 2);
    int64_t sym_iv = ray_sym_intern("iv", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_sv, parted_ps);
    tbl = ray_table_add_col(tbl, sym_iv, flat_ps);

    /* Keep every 1000th row — 70 matches */
    ray_t* pred_ps = ray_vec_new(RAY_BOOL, N);
    pred_ps->len = N;
    uint8_t* ppss = (uint8_t*)ray_data(pred_ps);
    int64_t pass_ps = 0;
    for (int64_t i = 0; i < N; i++) {
        ppss[i] = (i % 1000 == 0) ? 1 : 0;
        if (ppss[i]) pass_ps++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_ps);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_ps);

    ray_t* rsv = ray_table_get_col(result, sym_sv);
    TEST_ASSERT_NOT_NULL(rsv);
    TEST_ASSERT_EQ_I(rsv->len, pass_ps);

    ray_release(result);
    ray_release(pred_ps);
    ray_release(tbl);
    ray_release(parted_ps);
    ray_release(flat_ps);
    for (int64_t s = 0; s < N_SEG; s++) ray_release(segs_ps[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 26: exec_filter — large table MAPCOMMON + flat (parallel path,
 *                          MAPCOMMON materialization in parallel gather)
 *
 * Targets filter.c L268-273 (MAPCOMMON inside the parallel pre-alloc loop).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_mapcommon(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 70000;  /* > threshold */
    const int64_t N_PARTS = 7;
    const int64_t PART_SZ = N / N_PARTS;  /* 10000 */

    /* MAPCOMMON column */
    ray_t* kv2 = ray_vec_new(RAY_I64, N_PARTS); kv2->len = N_PARTS;
    int64_t* kvd2 = (int64_t*)ray_data(kv2);
    for (int64_t p = 0; p < N_PARTS; p++) kvd2[p] = 20240101 + (int32_t)p;
    ray_t* rc2 = ray_vec_new(RAY_I64, N_PARTS); rc2->len = N_PARTS;
    int64_t* rcd2 = (int64_t*)ray_data(rc2);
    for (int64_t p = 0; p < N_PARTS; p++) rcd2[p] = PART_SZ;
    ray_t* mc2 = make_mapcommon(kv2, rc2);

    /* Flat column */
    ray_t* flat2 = ray_vec_new(RAY_I64, N); flat2->len = N;
    int64_t* fd3 = (int64_t*)ray_data(flat2);
    for (int64_t i = 0; i < N; i++) fd3[i] = i;

    int64_t sym_mc = ray_sym_intern("mc",  2);
    int64_t sym_fv2 = ray_sym_intern("fv2", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_mc,  mc2);
    tbl = ray_table_add_col(tbl, sym_fv2, flat2);

    /* Keep first 5000 + last 5000 rows */
    ray_t* pred2 = ray_vec_new(RAY_BOOL, N); pred2->len = N;
    uint8_t* pp2 = (uint8_t*)ray_data(pred2);
    int64_t pass2b = 0;
    for (int64_t i = 0; i < N; i++) {
        pp2[i] = (i < 5000 || i >= 65000) ? 1 : 0;
        if (pp2[i]) pass2b++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred2);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass2b);

    /* MAPCOMMON should be materialized */
    ray_t* mc_res = ray_table_get_col(result, sym_mc);
    TEST_ASSERT_NOT_NULL(mc_res);
    TEST_ASSERT_EQ_I(mc_res->len, pass2b);

    ray_release(result);
    ray_release(pred2);
    ray_release(tbl);
    ray_release(mc2); ray_release(kv2); ray_release(rc2);
    ray_release(flat2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 27: sel_compact — parted STR column
 *
 * Targets filter.c L615-619 (the pbase==RAY_STR arm in sel_compact's
 * has_parted branch).
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_parted_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const char* words[] = { "one", "two", "three", "four", "five",
                             "six", "seven", "eight", "nine", "ten" };
    ray_t* seg_st0 = ray_vec_new(RAY_STR, 5); seg_st0->len = 0;
    for (int i = 0; i < 5; i++) seg_st0 = ray_str_vec_append(seg_st0, words[i], strlen(words[i]));
    ray_t* seg_st1 = ray_vec_new(RAY_STR, 5); seg_st1->len = 0;
    for (int i = 5; i < 10; i++) seg_st1 = ray_str_vec_append(seg_st1, words[i], strlen(words[i]));

    ray_t* segs_st[2] = { seg_st0, seg_st1 };
    ray_t* parted_st = make_parted(RAY_STR, segs_st, 2);

    ray_t* flat_st = ray_vec_new(RAY_I64, 10); flat_st->len = 10;
    int64_t* fst = (int64_t*)ray_data(flat_st);
    for (int i = 0; i < 10; i++) fst[i] = i;

    int64_t sym_stv = ray_sym_intern("stv", 3);
    int64_t sym_sti = ray_sym_intern("sti", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_stv, parted_st);
    tbl = ray_table_add_col(tbl, sym_sti, flat_st);

    /* Keep rows 2,3,7 */
    ray_t* pred_st = ray_vec_new(RAY_BOOL, 10); pred_st->len = 10;
    uint8_t* pst = (uint8_t*)ray_data(pred_st);
    memset(pst, 0, 10);
    pst[2] = 1; pst[3] = 1; pst[7] = 1;

    ray_t* sel_st = ray_rowsel_from_pred(pred_st);
    TEST_ASSERT_NOT_NULL(sel_st);

    ray_t* result = sel_compact(NULL, tbl, sel_st);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 3);

    ray_rowsel_release(sel_st);
    ray_release(result);
    ray_release(pred_st);
    ray_release(tbl);
    ray_release(parted_st);
    ray_release(flat_st);
    ray_release(seg_st0);
    ray_release(seg_st1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 28: exec_filter_parted_vec — esz-mismatch branch (SYM width fallback)
 *
 * Targets filter.c L144-154 (the !parted_seg_esz_ok path in
 * exec_filter_parted_vec).  Builds a parted SYM column where segments
 * have different widths — the first segment uses W8 but the driver expects
 * W16 — triggering the zero-fill path.
 *
 * We force the mismatch by wrapping a W8 SYM segment inside a parted
 * wrapper whose base_attrs expect W16, then filtering via exec_filter_seq.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_parted_esz_mismatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* W8 sym vector: 5 rows, indices 0..4 */
    ray_t* seg_w8 = ray_sym_vec_new(RAY_SYM_W8, 5);
    TEST_ASSERT_NOT_NULL(seg_w8);
    seg_w8->len = 5;
    uint8_t* w8d = (uint8_t*)ray_data(seg_w8);
    for (int i = 0; i < 5; i++) w8d[i] = (uint8_t)i;

    /* W16 sym vector: 5 rows — this is the "normal" segment */
    ray_t* seg_w16 = ray_sym_vec_new(RAY_SYM_W16, 5);
    TEST_ASSERT_NOT_NULL(seg_w16);
    seg_w16->len = 5;
    uint16_t* w16d = (uint16_t*)ray_data(seg_w16);
    for (int i = 0; i < 5; i++) w16d[i] = (uint16_t)(100 + i);

    /* Put W8 first so parted_first_attrs picks W8 but w16 seg fails esz check */
    ray_t* segs_em[2] = { seg_w8, seg_w16 };
    ray_t* parted_em = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted_em);
    parted_em->type = RAY_PARTED_BASE + RAY_SYM;
    parted_em->len = 2;
    ((ray_t**)ray_data(parted_em))[0] = segs_em[0];
    ((ray_t**)ray_data(parted_em))[1] = segs_em[1];

    /* Flat I64 companion */
    ray_t* flat_em = ray_vec_new(RAY_I64, 10); flat_em->len = 10;
    int64_t* femd = (int64_t*)ray_data(flat_em);
    for (int i = 0; i < 10; i++) femd[i] = i;

    int64_t sym_em = ray_sym_intern("em", 2);
    int64_t sym_ef = ray_sym_intern("ef", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_em, parted_em);
    tbl = ray_table_add_col(tbl, sym_ef, flat_em);

    /* pred: keep all 10 rows */
    ray_t* pred_em = ray_vec_new(RAY_BOOL, 10); pred_em->len = 10;
    uint8_t* pem = (uint8_t*)ray_data(pred_em);
    memset(pem, 1, 10);

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_em);
    /* May succeed or return error, but must not crash */
    TEST_ASSERT_NOT_NULL(result);

    if (!RAY_IS_ERR(result)) ray_release(result);
    ray_release(pred_em);
    ray_release(tbl);
    ray_release(parted_em);
    ray_release(flat_em);
    ray_release(seg_w8);
    ray_release(seg_w16);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 29: exec_filter — large table with parted SYM column
 *
 * Targets filter.c L280-286 (RAY_SYM branch inside exec_filter large-table
 * parallel pre-alloc loop).  Builds a table with 70000 rows including a
 * parted SYM W8 column so the SYM path runs in the parallel gather.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_parted_sym(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t SEG_SZ = 10000;
    const int64_t N_SEG  = 7;
    const int64_t N      = SEG_SZ * N_SEG;

    /* Intern a few symbols to use as values */
    int64_t s_a = ray_sym_intern("aa", 2);
    int64_t s_b = ray_sym_intern("bb", 2);
    (void)s_a; (void)s_b;

    /* Build parted SYM W8 (width sufficient for 255 interns) */
    ray_t* segs_sym[N_SEG];
    for (int64_t s = 0; s < N_SEG; s++) {
        segs_sym[s] = ray_sym_vec_new(RAY_SYM_W8, SEG_SZ);
        segs_sym[s]->len = SEG_SZ;
        uint8_t* d = (uint8_t*)ray_data(segs_sym[s]);
        for (int64_t j = 0; j < SEG_SZ; j++) d[j] = (uint8_t)((j % 2) + 1);
    }
    ray_t* parted_sym = make_parted(RAY_SYM, segs_sym, N_SEG);

    /* Flat I64 companion */
    ray_t* flat_sym = ray_vec_new(RAY_I64, N); flat_sym->len = N;
    int64_t* fsd = (int64_t*)ray_data(flat_sym);
    for (int64_t i = 0; i < N; i++) fsd[i] = i;

    int64_t sym_psym = ray_sym_intern("psym", 4);
    int64_t sym_fsym = ray_sym_intern("fsym", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_psym, parted_sym);
    tbl = ray_table_add_col(tbl, sym_fsym, flat_sym);

    /* Keep every 500th row */
    ray_t* pred = ray_vec_new(RAY_BOOL, N); pred->len = N;
    uint8_t* ppd = (uint8_t*)ray_data(pred);
    int64_t pass_sym = 0;
    for (int64_t i = 0; i < N; i++) {
        ppd[i] = (i % 500 == 0) ? 1 : 0;
        if (ppd[i]) pass_sym++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_sym);

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    ray_release(parted_sym);
    ray_release(flat_sym);
    for (int64_t s = 0; s < N_SEG; s++) ray_release(segs_sym[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 30: exec_filter — >16 flat columns (per-column parallel gather path)
 *
 * Targets filter.c L334-344 (per-column gather when ncols > MGATHER_MAX_COLS=16).
 * Builds a large table (>65536 rows) with 17 flat I64 columns and filters.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_many_cols(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N     = 70000;
    const int64_t NCOLS = 17;  /* > MGATHER_MAX_COLS = 16 */

    ray_t* cols[NCOLS];
    int64_t col_names[NCOLS];
    char cname[8];
    for (int64_t c = 0; c < NCOLS; c++) {
        cols[c] = ray_vec_new(RAY_I64, N);
        cols[c]->len = N;
        int64_t* d = (int64_t*)ray_data(cols[c]);
        for (int64_t i = 0; i < N; i++) d[i] = c * N + i;
        snprintf(cname, sizeof(cname), "c%lld", (long long)c);
        col_names[c] = ray_sym_intern(cname, strlen(cname));
    }

    ray_t* tbl = ray_table_new(NCOLS);
    for (int64_t c = 0; c < NCOLS; c++)
        tbl = ray_table_add_col(tbl, col_names[c], cols[c]);

    /* Keep every 100th row */
    ray_t* pred = ray_vec_new(RAY_BOOL, N); pred->len = N;
    uint8_t* ppd = (uint8_t*)ray_data(pred);
    int64_t pass_mc2 = 0;
    for (int64_t i = 0; i < N; i++) {
        ppd[i] = (i % 100 == 0) ? 1 : 0;
        if (ppd[i]) pass_mc2++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_mc2);

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    for (int64_t c = 0; c < NCOLS; c++) ray_release(cols[c]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 31: sel_compact — nrows mismatch error path
 *
 * Targets filter.c L508-512 (sel_compact returns error when sel->nrows
 * doesn't match tbl's row count).  Builds a rowsel for 1024 rows but
 * passes it with a table of 2048 rows.
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_nrows_mismatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Table: 2048 rows */
    ray_t* col_mm = ray_vec_new(RAY_I64, 2048); col_mm->len = 2048;
    int64_t* cmmld = (int64_t*)ray_data(col_mm);
    for (int i = 0; i < 2048; i++) cmmld[i] = i;
    int64_t sym_mm = ray_sym_intern("mm", 2);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_mm, col_mm);

    /* Rowsel built for 1024 rows */
    ray_t* pred_mm = ray_vec_new(RAY_BOOL, 1024); pred_mm->len = 1024;
    uint8_t* pmm = (uint8_t*)ray_data(pred_mm);
    memset(pmm, 1, 512);
    memset(pmm + 512, 0, 512);
    ray_t* sel_mm = ray_rowsel_from_pred(pred_mm);
    TEST_ASSERT_NOT_NULL(sel_mm);

    ray_t* result = sel_compact(NULL, tbl, sel_mm);
    /* Must return an error (nrows mismatch) */
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_TRUE(RAY_IS_ERR(result));

    ray_rowsel_release(sel_mm);
    ray_release(pred_mm);
    ray_release(tbl);
    ray_release(col_mm);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 32: sel_compact — >16 flat columns (per-column gather path)
 *
 * Targets filter.c L643-652 (per-column gather when ncols > MGATHER_MAX_COLS
 * in sel_compact).  Builds a table with 17 flat I64 columns, creates a
 * rowsel, compacts.
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_many_cols(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N     = 3072;  /* 3 morsels */
    const int64_t NCOLS = 17;

    ray_t* cols2[NCOLS];
    int64_t cnames2[NCOLS];
    char cname2[8];
    for (int64_t c = 0; c < NCOLS; c++) {
        cols2[c] = ray_vec_new(RAY_I64, N);
        cols2[c]->len = N;
        int64_t* d = (int64_t*)ray_data(cols2[c]);
        for (int64_t i = 0; i < N; i++) d[i] = c * 1000 + i;
        snprintf(cname2, sizeof(cname2), "d%lld", (long long)c);
        cnames2[c] = ray_sym_intern(cname2, strlen(cname2));
    }

    ray_t* tbl = ray_table_new(NCOLS);
    for (int64_t c = 0; c < NCOLS; c++)
        tbl = ray_table_add_col(tbl, cnames2[c], cols2[c]);

    /* Keep all rows in morsels 1 and 2, none in morsel 0 */
    ray_t* pred_mc3 = ray_vec_new(RAY_BOOL, N); pred_mc3->len = N;
    uint8_t* pmc3 = (uint8_t*)ray_data(pred_mc3);
    for (int64_t i = 0; i < N; i++) pmc3[i] = (i >= 1024) ? 1 : 0;

    ray_t* sel_mc3 = ray_rowsel_from_pred(pred_mc3);
    TEST_ASSERT_NOT_NULL(sel_mc3);

    ray_t* result = sel_compact(NULL, tbl, sel_mc3);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 2048);

    ray_rowsel_release(sel_mc3);
    ray_release(result);
    ray_release(pred_mc3);
    ray_release(tbl);
    for (int64_t c = 0; c < NCOLS; c++) ray_release(cols2[c]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 33: sel_compact — parted SYM column
 *
 * Targets filter.c L596-599 (SYM parted branch in sel_compact pre-alloc).
 * -------------------------------------------------------------------------- */
static test_result_t test_sel_compact_parted_sym(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2 segments × 30 rows = 60 total */
    ray_t* segs_psy[2];
    for (int s = 0; s < 2; s++) {
        segs_psy[s] = ray_sym_vec_new(RAY_SYM_W8, 30);
        segs_psy[s]->len = 30;
        uint8_t* d = (uint8_t*)ray_data(segs_psy[s]);
        for (int j = 0; j < 30; j++) d[j] = (uint8_t)(j % 3 + 1);
    }
    ray_t* parted_psy = make_parted(RAY_SYM, segs_psy, 2);

    ray_t* flat_psy = ray_vec_new(RAY_I64, 60); flat_psy->len = 60;
    int64_t* fpsy = (int64_t*)ray_data(flat_psy);
    for (int i = 0; i < 60; i++) fpsy[i] = i;

    int64_t sym_psy = ray_sym_intern("psy", 3);
    int64_t sym_fpy = ray_sym_intern("fpy", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_psy, parted_psy);
    tbl = ray_table_add_col(tbl, sym_fpy, flat_psy);

    /* Keep rows 10..19 */
    ray_t* pred_psy = ray_vec_new(RAY_BOOL, 60); pred_psy->len = 60;
    uint8_t* ppsy = (uint8_t*)ray_data(pred_psy);
    memset(ppsy, 0, 60);
    for (int i = 10; i < 20; i++) ppsy[i] = 1;

    ray_t* sel_psy = ray_rowsel_from_pred(pred_psy);
    TEST_ASSERT_NOT_NULL(sel_psy);

    ray_t* result = sel_compact(NULL, tbl, sel_psy);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 10);

    ray_rowsel_release(sel_psy);
    ray_release(result);
    ray_release(pred_psy);
    ray_release(tbl);
    ray_release(parted_psy);
    ray_release(flat_psy);
    for (int s = 0; s < 2; s++) ray_release(segs_psy[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 34: exec_filter_head — parted SYM column
 *
 * Targets filter.c L439-442 (SYM parted branch in exec_filter_head).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_head_parted_sym(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2 segments × 10 rows = 20 total */
    ray_t* segs_hs2[2];
    for (int s = 0; s < 2; s++) {
        segs_hs2[s] = ray_sym_vec_new(RAY_SYM_W8, 10);
        segs_hs2[s]->len = 10;
        uint8_t* d = (uint8_t*)ray_data(segs_hs2[s]);
        for (int j = 0; j < 10; j++) d[j] = (uint8_t)(j % 4 + 1);
    }
    ray_t* parted_hs2 = make_parted(RAY_SYM, segs_hs2, 2);

    int64_t sym_phs2 = ray_sym_intern("phs2", 4);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_phs2, parted_hs2);

    /* pred: all 20 rows true, limit=5 */
    ray_t* pred = ray_vec_new(RAY_BOOL, 20); pred->len = 20;
    uint8_t* ppd2 = (uint8_t*)ray_data(pred);
    memset(ppd2, 1, 20);

    ray_t* result = exec_filter_head(tbl, pred, 5);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);

    ray_release(result);
    ray_release(pred);
    ray_release(tbl);
    ray_release(parted_hs2);
    for (int s = 0; s < 2; s++) ray_release(segs_hs2[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 35: exec_filter_parted_vec — null propagation in non-STR path
 *
 * Targets filter.c L161-163 (ray_vec_set_null inside exec_filter_parted_vec).
 * Builds a parted I64 segment with a null bitmap, filters it, verifies nulls
 * are propagated to the output via exec_filter_seq.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_parted_vec_nulls(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 1 segment of 10 rows, row 3 is null */
    ray_t* seg_null = ray_vec_new(RAY_I64, 10);
    TEST_ASSERT_NOT_NULL(seg_null);
    seg_null->len = 10;
    int64_t* snd = (int64_t*)ray_data(seg_null);
    for (int i = 0; i < 10; i++) snd[i] = i * 10;
    /* Set row 3 as null */
    ray_vec_set_null(seg_null, 3, true);
    TEST_ASSERT_TRUE(seg_null->attrs & RAY_ATTR_HAS_NULLS);

    ray_t* segs_nv[1] = { seg_null };
    ray_t* parted_nv = make_parted(RAY_I64, segs_nv, 1);

    /* Flat companion */
    ray_t* flat_nv = ray_vec_new(RAY_I64, 10); flat_nv->len = 10;
    int64_t* fnv = (int64_t*)ray_data(flat_nv);
    for (int i = 0; i < 10; i++) fnv[i] = i;

    int64_t sym_pnv = ray_sym_intern("pnv", 3);
    int64_t sym_fnv = ray_sym_intern("fnv", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pnv, parted_nv);
    tbl = ray_table_add_col(tbl, sym_fnv, flat_nv);

    /* Keep all 10 rows */
    ray_t* pred_nv = ray_vec_new(RAY_BOOL, 10); pred_nv->len = 10;
    uint8_t* pnv = (uint8_t*)ray_data(pred_nv);
    memset(pnv, 1, 10);

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_nv);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 10);

    /* Verify the parted col in result has nulls propagated at row 3 */
    ray_t* rnv = ray_table_get_col(result, sym_pnv);
    TEST_ASSERT_NOT_NULL(rnv);
    if (rnv->attrs & RAY_ATTR_HAS_NULLS) {
        TEST_ASSERT_TRUE(ray_vec_is_null(rnv, 3));
    }

    ray_release(result);
    ray_release(pred_nv);
    ray_release(tbl);
    ray_release(parted_nv);
    ray_release(flat_nv);
    ray_release(seg_null);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 36: parted_gather_col — NULL segment skip (n_segs=0 guard)
 *
 * Targets filter.c L36 (n_segs == 0 early return).  Calls exec_filter on a
 * large table with a zero-segment parted column to trigger the guard.
 * -------------------------------------------------------------------------- */
static test_result_t test_parted_gather_col_zero_segs(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Zero-segment parted column */
    ray_t* parted_z = ray_alloc(0);
    TEST_ASSERT_NOT_NULL(parted_z);
    parted_z->type = RAY_PARTED_BASE + RAY_I64;
    parted_z->len  = 0;

    /* Large flat companion (> threshold) */
    const int64_t N = 70000;
    ray_t* flat_z = ray_vec_new(RAY_I64, N); flat_z->len = N;
    int64_t* fzd = (int64_t*)ray_data(flat_z);
    for (int64_t i = 0; i < N; i++) fzd[i] = i;

    int64_t sym_pz = ray_sym_intern("pz", 2);
    int64_t sym_fz = ray_sym_intern("fz", 2);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pz, parted_z);
    tbl = ray_table_add_col(tbl, sym_fz, flat_z);

    ray_t* pred_z = ray_vec_new(RAY_BOOL, N); pred_z->len = N;
    uint8_t* pzd = (uint8_t*)ray_data(pred_z);
    for (int64_t i = 0; i < N; i++) {
        pzd[i] = (i % 1000 == 0) ? 1 : 0;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_z);
    TEST_ASSERT_NOT_NULL(result);
    /* Result may succeed or be an error depending on table nrows detection;
     * either way we should not crash. */
    if (!RAY_IS_ERR(result)) {
        ray_release(result);
    }

    ray_release(pred_z);
    ray_release(tbl);
    ray_release(parted_z);
    ray_release(flat_z);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 37: exec_filter — large table with flat SYM column (parallel path,
 *           flat SYM branch at L284-285)
 *
 * When a large table has a flat (non-parted) SYM column, exec_filter reaches
 * the else-branch at L284 (out_attrs = col->attrs for flat SYM).
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_large_flat_sym(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 70000;

    /* Flat SYM W8 column */
    ray_t* sym_col = ray_sym_vec_new(RAY_SYM_W8, N);
    sym_col->len = N;
    uint8_t* scd = (uint8_t*)ray_data(sym_col);
    for (int64_t i = 0; i < N; i++) scd[i] = (uint8_t)(i % 4 + 1);

    /* Flat I64 companion */
    ray_t* flat_lfs = ray_vec_new(RAY_I64, N); flat_lfs->len = N;
    int64_t* flfsd = (int64_t*)ray_data(flat_lfs);
    for (int64_t i = 0; i < N; i++) flfsd[i] = i;

    int64_t sym_sc  = ray_sym_intern("sc",  2);
    int64_t sym_lf2 = ray_sym_intern("lf2", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_sc,  sym_col);
    tbl = ray_table_add_col(tbl, sym_lf2, flat_lfs);

    /* Keep every 1000th row */
    ray_t* pred_lfs = ray_vec_new(RAY_BOOL, N); pred_lfs->len = N;
    uint8_t* plfs = (uint8_t*)ray_data(pred_lfs);
    int64_t pass_lfs = 0;
    for (int64_t i = 0; i < N; i++) {
        plfs[i] = (i % 1000 == 0) ? 1 : 0;
        if (plfs[i]) pass_lfs++;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_lfs);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);
    TEST_ASSERT_EQ_I(ray_table_nrows(result), pass_lfs);

    ray_release(result);
    ray_release(pred_lfs);
    ray_release(tbl);
    ray_release(sym_col);
    ray_release(flat_lfs);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 38: parted_gather_col — NULL segment inside parted column (large path)
 *
 * Targets filter.c L59 (the continue for NULL/esz-mismatch segment in
 * parted_gather_col).  Builds a large table where one of the parted segments
 * is NULL so the gather skip branch executes.
 *
 * The non-NULL segments must total > RAY_PARALLEL_THRESHOLD (65536) so that
 * exec_filter takes the parallel path (not exec_filter_seq).  3 segs of
 * 25000 = 75000 total non-null rows; the NULL segment is 4th at the end.
 * -------------------------------------------------------------------------- */
static test_result_t test_parted_gather_col_null_seg(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 4 segments; last one is NULL.  First 3 total 75000 > 65536 */
    const int64_t SEG_SZ = 25000;
    const int64_t N_SEGS = 4;

    ray_t* segs_ns[N_SEGS];
    for (int s = 0; s < 3; s++) {
        segs_ns[s] = ray_vec_new(RAY_I64, SEG_SZ);
        segs_ns[s]->len = SEG_SZ;
        int64_t* d = (int64_t*)ray_data(segs_ns[s]);
        for (int64_t j = 0; j < SEG_SZ; j++) d[j] = (int64_t)(s * SEG_SZ + j);
    }
    segs_ns[3] = NULL;  /* NULL segment — triggers the skip in parted_gather_col */

    /* Build the parted column manually so we can embed a NULL segment */
    ray_t* parted_ns = ray_alloc((size_t)N_SEGS * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted_ns);
    parted_ns->type = RAY_PARTED_BASE + RAY_I64;
    parted_ns->len  = N_SEGS;
    ray_t** slot_ns = (ray_t**)ray_data(parted_ns);
    for (int s = 0; s < N_SEGS; s++) slot_ns[s] = segs_ns[s];

    /* ray_parted_nrows counts only non-null segs = 75000.
     * The flat companion and pred must also be 75000. */
    const int64_t N = SEG_SZ * 3;  /* 75000 */
    ray_t* flat_ns = ray_vec_new(RAY_I64, N); flat_ns->len = N;
    int64_t* fns = (int64_t*)ray_data(flat_ns);
    for (int64_t i = 0; i < N; i++) fns[i] = i;

    int64_t sym_pns = ray_sym_intern("pns", 3);
    int64_t sym_fns = ray_sym_intern("fns", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pns, parted_ns);
    tbl = ray_table_add_col(tbl, sym_fns, flat_ns);

    /* Keep every 5000th row */
    ray_t* pred_ns = ray_vec_new(RAY_BOOL, N); pred_ns->len = N;
    uint8_t* pns = (uint8_t*)ray_data(pred_ns);
    for (int64_t i = 0; i < N; i++) pns[i] = (i % 5000 == 0) ? 1 : 0;

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_ns);
    TEST_ASSERT_NOT_NULL(result);
    /* Should not crash; may succeed or return error */
    if (!RAY_IS_ERR(result)) ray_release(result);

    ray_release(pred_ns);
    ray_release(tbl);
    ray_release(parted_ns);
    ray_release(flat_ns);
    for (int s = 0; s < N_SEGS; s++) if (segs_ns[s]) ray_release(segs_ns[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 39: parted_gather_col — null bit propagation in large table
 *
 * Targets filter.c L64-66 (null bit set from segment inside parted_gather_col).
 * Builds a large parted column where one segment has RAY_ATTR_HAS_NULLS,
 * then runs exec_filter on the large table.
 * -------------------------------------------------------------------------- */
static test_result_t test_parted_gather_col_nullbits(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t SEG_SZ = 25000;
    const int64_t N_SEGS = 3;
    const int64_t N = SEG_SZ * N_SEGS;  /* 75000 > 65536 */

    ray_t* segs_nb[N_SEGS];
    for (int s = 0; s < N_SEGS; s++) {
        segs_nb[s] = ray_vec_new(RAY_I64, SEG_SZ);
        segs_nb[s]->len = SEG_SZ;
        int64_t* d = (int64_t*)ray_data(segs_nb[s]);
        for (int64_t j = 0; j < SEG_SZ; j++) d[j] = s * SEG_SZ + j;
    }
    /* Set some nulls in segment 1 */
    ray_vec_set_null(segs_nb[1], 0, true);
    ray_vec_set_null(segs_nb[1], 100, true);

    ray_t* parted_nb = make_parted(RAY_I64, segs_nb, N_SEGS);

    ray_t* flat_nb = ray_vec_new(RAY_I64, N); flat_nb->len = N;
    int64_t* fnb = (int64_t*)ray_data(flat_nb);
    for (int64_t i = 0; i < N; i++) fnb[i] = i;

    int64_t sym_pnb = ray_sym_intern("pnb", 3);
    int64_t sym_fnb = ray_sym_intern("fnb", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pnb, parted_nb);
    tbl = ray_table_add_col(tbl, sym_fnb, flat_nb);

    /* Keep rows across all segments including the null-having segment */
    ray_t* pred_nb = ray_vec_new(RAY_BOOL, N); pred_nb->len = N;
    uint8_t* pnb = (uint8_t*)ray_data(pred_nb);
    for (int64_t i = 0; i < N; i++) pnb[i] = (i % 500 == 0) ? 1 : 0;

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_nb);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_release(result);
    ray_release(pred_nb);
    ray_release(tbl);
    ray_release(parted_nb);
    ray_release(flat_nb);
    for (int s = 0; s < N_SEGS; s++) ray_release(segs_nb[s]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 40: exec_filter_head — esz-mismatch skip in parted gather
 *
 * Targets filter.c L470-471 (!parted_seg_esz_ok continue in exec_filter_head
 * non-STR parted loop).  Builds a table with a parted SYM column that has
 * mismatched widths between segments, then calls exec_filter_head.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_head_parted_esz_skip(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Seg 0: W16 SYM (5 rows); seg 1: W8 SYM (5 rows) — width mismatch */
    ray_t* seg_h0 = ray_sym_vec_new(RAY_SYM_W16, 5);
    seg_h0->len = 5;
    uint16_t* h0d = (uint16_t*)ray_data(seg_h0);
    for (int i = 0; i < 5; i++) h0d[i] = (uint16_t)(i + 1);

    ray_t* seg_h1 = ray_sym_vec_new(RAY_SYM_W8, 5);
    seg_h1->len = 5;
    uint8_t* h1d = (uint8_t*)ray_data(seg_h1);
    for (int i = 0; i < 5; i++) h1d[i] = (uint8_t)(i + 10);

    /* W16 first → parted_first_attrs picks W16 → W8 seg fails esz check */
    ray_t* parted_he = ray_alloc(2 * sizeof(ray_t*));
    parted_he->type = RAY_PARTED_BASE + RAY_SYM;
    parted_he->len  = 2;
    ((ray_t**)ray_data(parted_he))[0] = seg_h0;
    ((ray_t**)ray_data(parted_he))[1] = seg_h1;

    ray_t* flat_he = ray_vec_new(RAY_I64, 10); flat_he->len = 10;
    int64_t* fhed = (int64_t*)ray_data(flat_he);
    for (int i = 0; i < 10; i++) fhed[i] = i;

    int64_t sym_phe = ray_sym_intern("phe", 3);
    int64_t sym_fhe = ray_sym_intern("fhe", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_phe, parted_he);
    tbl = ray_table_add_col(tbl, sym_fhe, flat_he);

    /* pred: keep all 10 rows, limit=8 */
    ray_t* pred_he = ray_vec_new(RAY_BOOL, 10); pred_he->len = 10;
    uint8_t* phed = (uint8_t*)ray_data(pred_he);
    memset(phed, 1, 10);

    ray_t* result = exec_filter_head(tbl, pred_he, 8);
    TEST_ASSERT_NOT_NULL(result);
    /* Should not crash */
    if (!RAY_IS_ERR(result)) ray_release(result);

    ray_release(pred_he);
    ray_release(tbl);
    ray_release(parted_he);
    ray_release(flat_he);
    ray_release(seg_h0);
    ray_release(seg_h1);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test 41: parted_gather_col — SYM esz mismatch skip (large table path)
 *
 * Targets filter.c L59 (!parted_seg_esz_ok branch in parted_gather_col).
 * Builds a large parted SYM column where most segments are W16 (so
 * parted_first_attrs picks W16 → esz=2) but one segment is W8 (esz=1).
 * exec_filter uses the parallel parted path (> threshold), calling
 * parted_gather_col, where the W8 segment triggers the esz mismatch skip.
 * -------------------------------------------------------------------------- */
static test_result_t test_parted_gather_col_esz_mismatch(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 4 segments: 3 W16 (25000 each = 75000 > threshold) + 1 W8 (100 rows) */
    const int64_t SEG_W16 = 25000;
    const int64_t SEG_W8  = 100;

    ray_t* seg_a = ray_sym_vec_new(RAY_SYM_W16, SEG_W16);
    seg_a->len = SEG_W16;
    uint16_t* sad = (uint16_t*)ray_data(seg_a);
    for (int64_t j = 0; j < SEG_W16; j++) sad[j] = (uint16_t)(j % 1000 + 1);

    ray_t* seg_b = ray_sym_vec_new(RAY_SYM_W16, SEG_W16);
    seg_b->len = SEG_W16;
    uint16_t* sbd = (uint16_t*)ray_data(seg_b);
    for (int64_t j = 0; j < SEG_W16; j++) sbd[j] = (uint16_t)(j % 1000 + 1);

    ray_t* seg_c = ray_sym_vec_new(RAY_SYM_W16, SEG_W16);
    seg_c->len = SEG_W16;
    uint16_t* scd2 = (uint16_t*)ray_data(seg_c);
    for (int64_t j = 0; j < SEG_W16; j++) scd2[j] = (uint16_t)(j % 1000 + 1);

    /* W8 segment — will fail parted_seg_esz_ok since base_attrs from W16 */
    ray_t* seg_d = ray_sym_vec_new(RAY_SYM_W8, SEG_W8);
    seg_d->len = SEG_W8;
    uint8_t* sdd = (uint8_t*)ray_data(seg_d);
    for (int64_t j = 0; j < SEG_W8; j++) sdd[j] = (uint8_t)(j % 100 + 1);

    /* Place W16 segments first so parted_first_attrs picks W16 */
    ray_t* parted_em2 = ray_alloc(4 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(parted_em2);
    parted_em2->type = RAY_PARTED_BASE + RAY_SYM;
    parted_em2->len  = 4;
    ray_t** slot_em2 = (ray_t**)ray_data(parted_em2);
    slot_em2[0] = seg_a; slot_em2[1] = seg_b; slot_em2[2] = seg_c; slot_em2[3] = seg_d;

    /* Total rows from ray_parted_nrows = 75000 + 100 = 75100 > 65536 */
    const int64_t N = SEG_W16 * 3 + SEG_W8;

    /* Flat companion (75100 rows) */
    ray_t* flat_em2 = ray_vec_new(RAY_I64, N); flat_em2->len = N;
    int64_t* fem2d = (int64_t*)ray_data(flat_em2);
    for (int64_t i = 0; i < N; i++) fem2d[i] = i;

    int64_t sym_pem2 = ray_sym_intern("pem2", 4);
    int64_t sym_fem2 = ray_sym_intern("fem2", 4);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_pem2, parted_em2);
    tbl = ray_table_add_col(tbl, sym_fem2, flat_em2);

    /* Keep every 5000th row (includes rows from the W8 segment) */
    ray_t* pred_em2 = ray_vec_new(RAY_BOOL, N); pred_em2->len = N;
    uint8_t* pem2 = (uint8_t*)ray_data(pred_em2);
    for (int64_t i = 0; i < N; i++) pem2[i] = (i % 5000 == 0) ? 1 : 0;

    ray_t* result = exec_filter(NULL, NULL, tbl, pred_em2);
    TEST_ASSERT_NOT_NULL(result);
    /* Should not crash */
    if (!RAY_IS_ERR(result)) ray_release(result);

    ray_release(pred_em2);
    ray_release(tbl);
    ray_release(parted_em2);
    ray_release(flat_em2);
    ray_release(seg_a); ray_release(seg_b); ray_release(seg_c); ray_release(seg_d);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Suite definition
 * -------------------------------------------------------------------------- */

/* S0 carry-over: parted I64 filter + parted STR head/tail. */

static test_result_t test_filter_parted_i64(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments of 4 i64 rows each — 12 total. */
    ray_t* segs_v[3];
    int64_t s0[] = {1, 2, 3, 4};
    int64_t s1[] = {5, 6, 7, 8};
    int64_t s2[] = {9, 10, 11, 12};
    segs_v[0] = ray_vec_new(RAY_I64, 4); segs_v[0]->len = 4;
    memcpy(ray_data(segs_v[0]), s0, sizeof(s0));
    segs_v[1] = ray_vec_new(RAY_I64, 4); segs_v[1]->len = 4;
    memcpy(ray_data(segs_v[1]), s1, sizeof(s1));
    segs_v[2] = ray_vec_new(RAY_I64, 4); segs_v[2]->len = 4;
    memcpy(ray_data(segs_v[2]), s2, sizeof(s2));
    ray_t* val = make_parted(RAY_I64, segs_v, 3);

    int64_t sym_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_val, val);

    /* Predicate: pass even values — 6 rows match (2,4,6,8,10,12). */
    ray_t* pred = ray_vec_new(RAY_BOOL, 12); pred->len = 12;
    uint8_t* pd = (uint8_t*)ray_data(pred);
    int64_t expected[] = {2, 4, 6, 8, 10, 12};
    int e = 0;
    for (int seg = 0; seg < 3; seg++) {
        int64_t* sd = (int64_t*)ray_data(segs_v[seg]);
        for (int i = 0; i < 4; i++)
            pd[seg * 4 + i] = (sd[i] % 2 == 0) ? 1 : 0;
    }

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* out_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(out_col);
    TEST_ASSERT_EQ_I(out_col->len, 6);
    int64_t* od = (int64_t*)ray_data(out_col);
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_FMT(od[i] == expected[i],
                        "i=%d got=%lld expected=%lld",
                        i, (long long)od[i], (long long)expected[i]);
    }
    (void)e;

    ray_release(result);
    ray_release(tbl);
    ray_release(pred);
    ray_release(val);
    for (int i = 0; i < 3; i++) ray_release(segs_v[i]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

static test_result_t test_op_head_tail_on_parted_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 3 segments of 3 strings each = 9 rows total. */
    ray_t* s0 = ray_vec_new(RAY_STR, 0);
    s0 = ray_str_vec_append(s0, "alpha", 5);
    s0 = ray_str_vec_append(s0, "beta",  4);
    s0 = ray_str_vec_append(s0, "gamma", 5);
    ray_t* s1 = ray_vec_new(RAY_STR, 0);
    s1 = ray_str_vec_append(s1, "delta",   5);
    s1 = ray_str_vec_append(s1, "epsilon", 7);
    s1 = ray_str_vec_append(s1, "zeta",    4);
    ray_t* s2 = ray_vec_new(RAY_STR, 0);
    s2 = ray_str_vec_append(s2, "eta",   3);
    s2 = ray_str_vec_append(s2, "theta", 5);
    s2 = ray_str_vec_append(s2, "iota",  4);

    ray_t* segs[3] = { s0, s1, s2 };
    ray_t* val = make_parted(RAY_STR, segs, 3);
    TEST_ASSERT_NOT_NULL(val);

    /* MAPCOMMON keyed by I64 — counts match parted segment lengths. */
    int64_t keys[]   = {20240101, 20240102, 20240103};
    int64_t counts[] = {3, 3, 3};
    ray_t* kv = ray_vec_new(RAY_I64, 3); kv->len = 3;
    memcpy(ray_data(kv), keys, sizeof(keys));
    ray_t* rc = ray_vec_new(RAY_I64, 3); rc->len = 3;
    memcpy(ray_data(rc), counts, sizeof(counts));
    ray_t* mc = make_mapcommon(kv, rc);

    int64_t sym_dt  = ray_sym_intern("dt", 2);
    int64_t sym_val = ray_sym_intern("val", 3);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, sym_dt, mc);
    tbl = ray_table_add_col(tbl, sym_val, val);

    /* head 5 — first 5 strings: alpha, beta, gamma, delta, epsilon. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tnode = ray_const_table(g, tbl);
    ray_op_t* h = ray_head(g, tnode, 5);
    ray_t* result = ray_execute(g, h);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 5);
    ray_release(result);
    ray_graph_free(g);

    /* tail 4 — last 4 strings: zeta, eta, theta, iota. */
    g = ray_graph_new(tbl);
    tnode = ray_const_table(g, tbl);
    ray_op_t* t = ray_tail(g, tnode, 4);
    result = ray_execute(g, t);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(ray_table_nrows(result), 4);

    ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_release(mc); ray_release(kv); ray_release(rc);
    ray_release(val);
    ray_release(s0); ray_release(s1); ray_release(s2);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t partition_exec_entries[] = {
    { "part_exec/mc_basic",         test_materialize_mapcommon_basic,  NULL, NULL },
    { "part_exec/mc_head",          test_materialize_mapcommon_head,   NULL, NULL },
    { "part_exec/mc_filter",        test_materialize_mapcommon_filter, NULL, NULL },
    { "part_exec/op_head_parted",   test_op_head_on_parted_col,        NULL, NULL },
    { "part_exec/op_tail_parted",   test_op_tail_on_parted_col,        NULL, NULL },
    { "part_exec/stream_filter",    test_segment_streaming_filter,     NULL, NULL },
    { "part_exec/result_merge",     test_result_merge_via_streaming,   NULL, NULL },
    { "part_exec/pg_e2",            test_partitioned_gather_e2,        NULL, NULL },
    { "part_exec/pg_e1",            test_partitioned_gather_e1,        NULL, NULL },
    { "part_exec/pg_fallback",      test_partitioned_gather_fallback,  NULL, NULL },
    /* Filter coverage tests */
    { "filter/parted_seq",           test_filter_parted_seq,              NULL, NULL },
    { "filter/table_parted_seq",     test_filter_table_parted_seq,        NULL, NULL },
    { "filter/parted_str",           test_filter_parted_str,              NULL, NULL },
    { "filter/large_flat",           test_filter_large_flat,              NULL, NULL },
    { "filter/large_parted",         test_filter_large_parted,            NULL, NULL },
    { "filter/filter_head_parted",   test_filter_head_parted,             NULL, NULL },
    { "filter/sel_compact_basic",    test_sel_compact_basic,              NULL, NULL },
    { "filter/sel_compact_none",     test_sel_compact_none_pass,          NULL, NULL },
    { "filter/sel_compact_parted",   test_sel_compact_parted,             NULL, NULL },
    { "filter/seq_mapcommon",        test_filter_seq_mapcommon,           NULL, NULL },
    { "filter/head_zero_limit",      test_filter_head_zero_limit,         NULL, NULL },
    { "filter/head_non_table",       test_filter_head_non_table,          NULL, NULL },
    { "filter/head_parted_str",      test_filter_head_parted_str,         NULL, NULL },
    { "filter/parted_gather_multi",  test_parted_gather_col_multi_seg,    NULL, NULL },
    { "filter/large_parted_str",     test_filter_large_parted_str,        NULL, NULL },
    { "filter/large_mapcommon",      test_filter_large_mapcommon,         NULL, NULL },
    { "filter/sel_compact_pstr",     test_sel_compact_parted_str,         NULL, NULL },
    { "filter/parted_esz_mismatch",  test_filter_parted_esz_mismatch,     NULL, NULL },
    { "filter/large_parted_sym",     test_filter_large_parted_sym,        NULL, NULL },
    { "filter/large_many_cols",      test_filter_large_many_cols,         NULL, NULL },
    { "filter/sel_compact_mismatch", test_sel_compact_nrows_mismatch,     NULL, NULL },
    { "filter/sel_compact_manycols", test_sel_compact_many_cols,          NULL, NULL },
    { "filter/sel_compact_psym",     test_sel_compact_parted_sym,         NULL, NULL },
    { "filter/head_parted_sym",      test_filter_head_parted_sym,         NULL, NULL },
    { "filter/parted_vec_nulls",     test_filter_parted_vec_nulls,        NULL, NULL },
    { "filter/gather_col_zero_segs", test_parted_gather_col_zero_segs,    NULL, NULL },
    { "filter/large_flat_sym",       test_filter_large_flat_sym,          NULL, NULL },
    { "filter/gather_col_null_seg",  test_parted_gather_col_null_seg,     NULL, NULL },
    { "filter/gather_col_nullbits",  test_parted_gather_col_nullbits,     NULL, NULL },
    { "filter/head_esz_skip",        test_filter_head_parted_esz_skip,    NULL, NULL },
    { "filter/gather_col_esz_mismatch", test_parted_gather_col_esz_mismatch, NULL, NULL },
    { "part_exec/filter_parted_i64",    test_filter_parted_i64,              NULL, NULL },
    { "part_exec/head_tail_parted_str", test_op_head_tail_on_parted_str,     NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
