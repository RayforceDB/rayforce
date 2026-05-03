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
#include "table/sym.h"
#include "core/pool.h"
#include <string.h>

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
 * Test: exec_filter on a small parted I64 table — drives exec_filter_seq
 * → exec_filter_parted_vec (the non-STR branch at filter.c:131-167).
 * Small (12 rows total, 3 segments) so the parallel-gather path is
 * skipped via the RAY_PARALLEL_THRESHOLD fallback in exec_filter.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * Test: exec_filter on a small parted RAY_STR table — drives the STR
 * branch in exec_filter_parted_vec (filter.c:111-129) via exec_filter_seq.
 * -------------------------------------------------------------------------- */
static test_result_t test_filter_parted_str(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* 2 segments of 3 strings each — 6 total. */
    const char* w0[] = {"alpha", "beta", "gamma"};
    const char* w1[] = {"delta", "epsilon", "zeta"};

    ray_t* segs_v[2];
    segs_v[0] = ray_vec_new(RAY_STR, 0);
    for (int i = 0; i < 3; i++)
        segs_v[0] = ray_str_vec_append(segs_v[0], w0[i], strlen(w0[i]));
    segs_v[1] = ray_vec_new(RAY_STR, 0);
    for (int i = 0; i < 3; i++)
        segs_v[1] = ray_str_vec_append(segs_v[1], w1[i], strlen(w1[i]));

    ray_t* val = make_parted(RAY_STR, segs_v, 2);

    int64_t sym_val = ray_sym_intern("s", 1);
    ray_t* tbl = ray_table_new(1);
    tbl = ray_table_add_col(tbl, sym_val, val);

    /* Predicate: pick rows 1, 2, 4 — "beta", "gamma", "epsilon". */
    ray_t* pred = ray_vec_new(RAY_BOOL, 6); pred->len = 6;
    uint8_t* pd = (uint8_t*)ray_data(pred);
    pd[0]=0; pd[1]=1; pd[2]=1; pd[3]=0; pd[4]=1; pd[5]=0;

    ray_t* result = exec_filter(NULL, NULL, tbl, pred);
    TEST_ASSERT_NOT_NULL(result);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT_EQ_I(result->type, RAY_TABLE);

    ray_t* out_col = ray_table_get_col_idx(result, 0);
    TEST_ASSERT_NOT_NULL(out_col);
    TEST_ASSERT_EQ_I(out_col->len, 3);

    ray_release(result);
    ray_release(tbl);
    ray_release(pred);
    ray_release(val);
    for (int i = 0; i < 2; i++) ray_release(segs_v[i]);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* --------------------------------------------------------------------------
 * Test: OP_HEAD / OP_TAIL on a parted RAY_STR table — drives the
 * parted-STR helpers in src/ops/internal.h: parted_head_str,
 * parted_tail_str, parted_str_single_pool, col_propagate_str_pool_parted.
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * Suite definition
 * -------------------------------------------------------------------------- */

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
    { "part_exec/filter_parted_i64", test_filter_parted_i64,            NULL, NULL },
    { "part_exec/filter_parted_str", test_filter_parted_str,            NULL, NULL },
    { "part_exec/head_tail_parted_str", test_op_head_tail_on_parted_str, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
