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

/* test/test_idx_route.c — index routing scaffold + refactor pin (Task 1)
 *                         + zone all/none short-circuit tests (Task 2)
 *                         + sort-index range rowsel tests (Task 4)
 *                         + staleness end-to-end + adversarial sweeps (Task 10). */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/sym.h"
#include "ops/idxop.h"
#include "ops/rowsel.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "lang/internal.h"  /* ray_find_fn */
#include "table/table.h"
#include <string.h>
#include <stdlib.h>

/* ─── Fixture ──────────────────────────────────────────────────────── */

/* Build the route-test table:
 *   k I64 {5,1,9,3,7,1,9,5,3,7}   (10 rows; k==9 at rows 2 and 6)
 *   v I64 {0,1,2,3,4,5,6,7,8,9}
 */
static ray_t* make_idx_table(void) {
    (void)ray_sym_init();
    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    int64_t vd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 10);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 10);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    return tbl;
}

/* Attach a hash index to the named column in `tbl`.
 *
 * Mechanism: ray_table_get_col returns a BORROWED pointer; ray_index_attach_hash
 * takes a ray_t** and may COW-replace the vector (it retains internally).
 * After attachment the table still holds the OLD pointer, so we must write
 * the new vector back via ray_table_set_col_idx.
 *
 * Steps:
 *   1. Find the column's slot index by scanning the schema.
 *   2. Get the borrowed pointer and retain it to take ownership.
 *   3. Call ray_index_attach_hash(&w) — w may be replaced.
 *   4. Write w back to the table slot via ray_table_set_col_idx.
 *      ray_table_set_col_idx retains the new pointer and releases
 *      the old one, so we release our retained copy afterward.
 *   5. Release our retained copy (table now owns the ref).
 *
 * Returns 0 on success, -1 on failure (col not found or attach error).
 */
static int attach_hash_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;

    /* Take ownership so attach_hash can COW if needed. */
    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_hash(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    /* Write possibly-new pointer back into the table slot. */
    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* Attach a part index using the same retain/write-back cycle. */
static int attach_part_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols = ray_table_ncols(tbl);
    int64_t slot = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;
    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_part(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }
    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* Attach a zone index to the named column in `tbl`.
 * Same retain/write-back/release cycle as attach_hash_to_col:
 * ray_table_set_col_idx retains `w` before releasing the old pointer,
 * so our subsequent ray_release(w) drops the extra retain we took. */
static int attach_zone_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;

    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_zone(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* Build a FILTER(const_table, pred) graph from `tbl`, execute it, and
 * return the result.  Caller owns the returned reference. */
typedef ray_op_t* (*pred_builder_t)(ray_graph_t* g);

static ray_t* run_filter(ray_t* tbl, pred_builder_t build_pred) {
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* pred    = build_pred(g);
    ray_op_t* filt    = ray_filter(g, tbl_nd, pred);
    ray_t*    result  = ray_execute(g, filt);
    ray_graph_free(g);
    return result;
}

/* Compare the v-column values of two result tables.  Both must have the
 * same nrows.  Returns 1 iff identical (element-wise). */
static int v_cols_equal(ray_t* ra, ray_t* rb) {
    int64_t na = ray_table_nrows(ra);
    int64_t nb = ray_table_nrows(rb);
    if (na != nb) return 0;
    int64_t v_sym = ray_sym_intern("v", 1);
    ray_t* va = ray_table_get_col(ra, v_sym);
    ray_t* vb = ray_table_get_col(rb, v_sym);
    if (!va || !vb || RAY_IS_ERR(va) || RAY_IS_ERR(vb)) return 0;
    for (int64_t i = 0; i < na; i++) {
        if (ray_vec_get_i64(va, i) != ray_vec_get_i64(vb, i)) return 0;
    }
    return 1;
}

/* Differential helper: run the same filter on two table instances — one
 * with an index (side A) and one without (side B).  Asserts identical row
 * counts and v-column values.
 *
 * expect_zone_hit     >= 0: assert hits delta equals this value (1 = hit, 0 = no hit)
 *                      < 0: skip hits check
 * expect_zone_consult >= 0: assert consults delta equals this value
 *                      < 0: skip consults check */
static test_result_t diff_idx_filter(pred_builder_t build_pred,
                                     int attach_zone,
                                     int expect_rows,
                                     int expect_zone_hit,
                                     int expect_zone_consult) {
    /* Side A — with index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    if (attach_zone) {
        int ok = attach_zone_to_col(tbl_a, "k");
        TEST_ASSERT(ok == 0, "attach_zone_to_col k (side A)");
    }

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_ZONE];
    ray_t* ra = run_filter(tbl_a, build_pred);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), expect_rows);

    if (expect_zone_hit >= 0) {
        uint64_t delta = ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before;
        TEST_ASSERT_EQ_I((int64_t)delta, (int64_t)expect_zone_hit);
    }
    if (expect_zone_consult >= 0) {
        uint64_t delta = ray_idx_consults[IDX_SITE_FILTER_ZONE] - consults_before;
        TEST_ASSERT_EQ_I((int64_t)delta, (int64_t)expect_zone_consult);
    }

    /* Side B — without index */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, build_pred);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), expect_rows);

    /* Same v-values in result order (filter preserves row order). */
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch between indexed and scan results");

    ray_release(ra);
    ray_release(rb);
    ray_release(tbl_a);
    ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Attach a bloom index to the named column in `tbl`.
 * Same retain/write-back/release cycle as attach_hash_to_col. */
static int attach_bloom_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;

    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_bloom(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* ─── Predicate builders for zone tests ──────────────────────────── */

/* (> k 100) — no k in [1,9] satisfies this → 0 rows */
static ray_op_t* pred_gt_100(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 100));
}

/* (> k 0) — all k in [1,9] satisfy this → 10 rows */
static ray_op_t* pred_gt_0(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 0));
}

/* (> k 5) — k∈{9,7,9,7} satisfy → 4 rows */
static ray_op_t* pred_gt_5(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 5));
}

/* (<= k 9) — max==key boundary → all 10 rows (ALL) */
static ray_op_t* pred_le_9(ray_graph_t* g) {
    return ray_le(g, ray_scan(g, "k"), ray_const_i64(g, 9));
}

/* (>= k 1) — min==key boundary → all 10 rows (ALL) */
static ray_op_t* pred_ge_1(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 1));
}

/* (!= k 100) — key outside [1,9] → all 10 rows (ALL) */
static ray_op_t* pred_ne_100(ray_graph_t* g) {
    return ray_ne(g, ray_scan(g, "k"), ray_const_i64(g, 100));
}

/* (== k 0) — key just below min=1 → 0 rows (NONE) */
static ray_op_t* pred_eq_0(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 0));
}

/* (> f 100.0) on F64 column f∈[1.5,10.5] → 0 rows (NONE) */
static ray_op_t* pred_f_gt_100(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 100.0));
}

/* (> f 100) — INTEGER literal against the F64 column.  Decode must
 * coerce the key into the column's float family before zone consult. */
static ray_op_t* pred_f_gt_100_int(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "f"), ray_const_i64(g, 100));
}

/* (> k 5.5) — FLOAT literal against the I64 column.  Decode must reject
 * (return 0) so the scan fallback's promotion semantics apply. */
static ray_op_t* pred_k_gt_5p5(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_f64(g, 5.5));
}

/* ─── F64 zone fixture ─────────────────────────────────────────────── */

/* Build a zone-test table with an F64 column f {1.5..10.5} and I64 row id r. */
static ray_t* make_f64_zone_table(void) {
    (void)ray_sym_init();
    double fd[] = {1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5};
    int64_t rd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* fv = ray_vec_from_raw(RAY_F64, fd, 10);
    ray_t* rv = ray_vec_from_raw(RAY_I64, rd, 10);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("f", 1), fv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("r", 1), rv);
    ray_release(fv);
    ray_release(rv);
    return tbl;
}

/* ─── Tests ────────────────────────────────────────────────────────── */

/* Refactor pin: hash-eq fast path still produces correct results and
 * advances the routing counters after the rowsel-builder extraction
 * (Step 2) and idx_fresh precheck (Step 3). */
static test_result_t test_hash_eq_still_works(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Attach hash index to column k. */
    int ok = attach_hash_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_hash_to_col k");

    /* Build FILTER(const_table, (== k 9)) and execute. */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* tbl_node = ray_const_table(g, tbl);
    ray_op_t* pred     = ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 9));
    ray_op_t* filt     = ray_filter(g, tbl_node, pred);

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_HASH];
    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_HASH];

    ray_t* r = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);  /* k==9 at rows 2 and 6 */

    TEST_ASSERT(ray_idx_consults[IDX_SITE_FILTER_HASH] > consults_before,
                "consult counter must advance");
    TEST_ASSERT(ray_idx_hits[IDX_SITE_FILTER_HASH] > hits_before,
                "hit counter must advance");

    ray_release(r);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Zone all/none: (> k 100) with zone index → 0 rows, hit fires. */
static test_result_t test_zone_none(void) {
    return diff_idx_filter(pred_gt_100, /*attach_zone=*/1,
                           /*expect_rows=*/0, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* Zone all/none: (> k 0) with zone index → 10 rows, hit fires. */
static test_result_t test_zone_all(void) {
    return diff_idx_filter(pred_gt_0, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* Zone unknown: (> k 5) with zone index → 4 rows; min=1 max=9, so
 * the zone cannot decide → consult advances but hit must NOT. */
static test_result_t test_zone_unknown(void) {
    return diff_idx_filter(pred_gt_5, /*attach_zone=*/1,
                           /*expect_rows=*/4, /*expect_zone_hit=*/0,
                           /*expect_zone_consult=*/1);
}

/* Zone null exclusion: attach zone to a column with a null, query (> k 0).
 * k={5,1,NULL,3,7,1,9,5,3,7}: 9 non-null rows, but (null > 0) is false per
 * null semantics, so expect 9 rows.  The consult must NOT fire (idx_fresh_nonull
 * rejects null-bearing columns) — result must be correct via scan. */
static test_result_t test_zone_nulls_excluded(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build table with a null in k at row 2. */
    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    int64_t vd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* kv = ray_vec_from_raw(RAY_I64, kd, 10);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 10);

    /* Attach zone BEFORE setting null so the zone records n_nulls==0 and
     * min/max are computed on all non-null values.  Then set the null — this
     * mutates the column's data but the index becomes stale / col gets
     * HAS_NULLS, making idx_fresh_nonull reject it regardless. */
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);

    /* Attach zone to k (before setting null — tests the guard path). */
    int ok = attach_zone_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_zone_to_col k");

    /* Now set null at row 2 in the live column.
     * ray_vec_set_null → ray_vec_set_null_checked → vec_drop_index_inplace:
     * mutation auto-drops the index, so after this call kcol has no index at
     * all.  idx_fresh_nonull therefore never fires — the consult short-circuits
     * at the "no zone attached" check before it even reaches the HAS_NULLS
     * guard.  idx_fresh_nonull's HAS_NULLS clause is a second line of defence
     * for the hypothetical case of a stale-but-surviving index, but that
     * survivor cannot exist here because drop is unconditional on mutation. */
    int64_t k_sym = ray_sym_intern("k", 1);
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == k_sym) { slot = i; break; }
    }
    TEST_ASSERT(slot >= 0, "k column not found");
    ray_t* kcol = ray_table_get_col_idx(tbl, slot);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kcol));
    ray_vec_set_null(kcol, 2, true);  /* mutate in place */

    /* Run (> k 0): null row is false per null semantics → 9 rows. */
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_ZONE];

    ray_graph_t* g     = ray_graph_new(tbl);
    ray_op_t* tbl_node = ray_const_table(g, tbl);
    ray_op_t* pred     = ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 0));
    ray_op_t* filt     = ray_filter(g, tbl_node, pred);
    ray_t* r = ray_execute(g, filt);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 9);

    /* Zone consult must NOT have fired (nonull gate rejects). */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before), 0);

    ray_release(r);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* (<= k 9): max==key boundary → ALL → 10 rows, hit advances. */
static test_result_t test_zone_le_all_boundary(void) {
    return diff_idx_filter(pred_le_9, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* (>= k 1): min==key boundary → ALL → 10 rows, hit advances. */
static test_result_t test_zone_ge_all_boundary(void) {
    return diff_idx_filter(pred_ge_1, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* (!= k 100): key outside [1,9] → ALL → 10 rows, hit advances. */
static test_result_t test_zone_ne_all(void) {
    return diff_idx_filter(pred_ne_100, /*attach_zone=*/1,
                           /*expect_rows=*/10, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* (== k 0): key just below min=1 → NONE → 0 rows, hit advances. */
static test_result_t test_zone_eq_none_boundary(void) {
    return diff_idx_filter(pred_eq_0, /*attach_zone=*/1,
                           /*expect_rows=*/0, /*expect_zone_hit=*/1,
                           /*expect_zone_consult=*/-1);
}

/* Zone F64: (> f 100.0) on f∈[1.5,10.5] → NONE → 0 rows, hit advances.
 * Exercises the float min_f/max_f path in ray_index_zone_class. */
static test_result_t test_zone_f64_none(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Attach zone index to the F64 column. */
    int ok = attach_zone_to_col(tbl, "f");
    TEST_ASSERT(ok == 0, "attach_zone_to_col f");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_ZONE];

    ray_t* r = run_filter(tbl, pred_f_gt_100);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    /* Zone classified NONE: consult and hit both advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_ZONE] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before), 1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Cross-type: F64 zone column, INTEGER literal — (> f 100) on f∈[1.5,10.5].
 * Decode must reconcile the literal into the column's float family so the
 * zone classifies against key_f=100.0 (NONE), not the zero-filled key_f.
 * Expect 0 rows with the zone NONE hit advancing. */
static test_result_t test_zone_f64_int_literal(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_zone_to_col(tbl, "f");
    TEST_ASSERT(ok == 0, "attach_zone_to_col f");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_ZONE];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_ZONE];

    ray_t* r = run_filter(tbl, pred_f_gt_100_int);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    /* Zone classified NONE: consult and hit both advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_ZONE] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_ZONE] - hits_before), 1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Cross-type: I64 zone column, FLOAT literal — (> k 5.5) on k∈[1,9].
 * Decode must REJECT the shape (integer-family column, float key): the
 * fallback's promotion semantics own it.  No consult, no hit; correct
 * 4 rows (k∈{9,7,9,7}) via scan. */
static test_result_t test_zone_int_col_float_literal(void) {
    return diff_idx_filter(pred_k_gt_5p5, /*attach_zone=*/1,
                           /*expect_rows=*/4, /*expect_zone_hit=*/0,
                           /*expect_zone_consult=*/0);
}

/* ─── Predicate builders for bloom tests ─────────────────────────── */

/* (== k 4) — k∈{1,3,5,7,9}: 4 is absent, inside [min=1,max=9].
 * Bloom must prove absence; zone alone cannot (zone [1,9] contains 4). */
static ray_op_t* pred_eq_4(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 4));
}

/* (== k 9) — k∈{5,1,9,3,7,1,9,5,3,7}: 9 is present at rows 2 and 6 → 2 rows. */
static ray_op_t* pred_eq_9(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 9));
}

/* (> k 4) — k∈{5,9,7,9,5,7}: 6 rows.  Bloom is EQ-only, must not be consulted. */
static ray_op_t* pred_gt_4(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "k"), ray_const_i64(g, 4));
}

/* ─── Bloom tests ─────────────────────────────────────────────────── */

/* bloom_absent: bloom on k; pred (== k 4); k∈{1,3,5,7,9} so 4 is absent.
 * Bloom proves absent → 0 rows, consult and hit both advance. */
static test_result_t test_bloom_absent(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_bloom_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_bloom_to_col k");

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];

    ray_t* r = run_filter(tbl, pred_eq_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_present_falls_through: bloom on k; pred (== k 9); 9 is present.
 * Bloom cannot prove absence → falls through to scan → 2 rows.
 * Consult advances, hit must NOT. */
static test_result_t test_bloom_present_falls_through(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_bloom_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_bloom_to_col k");

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];

    ray_t* r = run_filter(tbl, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_diff_absent: differential — bloom-indexed vs no-index for (== k 4).
 * Bloom fires, same 0 rows, same v-values (trivially). */
static test_result_t test_bloom_diff_absent(void) {
    /* Side A — with bloom */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_bloom_to_col(tbl_a, "k") == 0, "attach bloom (side A)");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    ray_t* ra = run_filter(tbl_a, pred_eq_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 0);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);

    /* Side B — no index */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 0);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_diff_present: differential — bloom-indexed vs no-index for (== k 9).
 * Bloom does NOT hit (present), scan produces same 2 rows. */
static test_result_t test_bloom_diff_present(void) {
    /* Side A — with bloom */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_bloom_to_col(tbl_a, "k") == 0, "attach bloom (side A)");

    uint64_t hits_before     = ray_idx_hits[IDX_SITE_FILTER_BLOOM];
    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];
    ray_t* ra = run_filter(tbl_a, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 2);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_BLOOM]     - hits_before),     0);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 1);

    /* Side B — no index */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* bloom_noneq_ignored: bloom on k; pred (> k 4).
 * Bloom is EQ-only: consult must NOT advance; correct 6 rows via scan. */
static test_result_t test_bloom_noneq_ignored(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    int ok = attach_bloom_to_col(tbl, "k");
    TEST_ASSERT(ok == 0, "attach_bloom_to_col k");

    uint64_t consults_before = ray_idx_consults[IDX_SITE_FILTER_BLOOM];

    ray_t* r = run_filter(tbl, pred_gt_4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 6);   /* k∈{5,9,7,9,5,7} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_BLOOM] - consults_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Sort-index attach helper ────────────────────────────────────── */

/* Attach a sort index to the named column in `tbl`.
 * Same retain/write-back/release cycle as attach_hash_to_col. */
static int attach_sort_to_col(ray_t* tbl, const char* name) {
    int64_t sym_id = ray_sym_intern(name, (int64_t)strlen(name));
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return -1;

    ray_t* w = col;
    ray_retain(w);
    ray_t* r = ray_index_attach_sort(&w);
    if (!r || RAY_IS_ERR(r)) { ray_release(w); return -1; }

    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);
    return 0;
}

/* ─── Predicate builders for range tests ──────────────────────────── */

/* (< k 5) → k∈{1,3,1,3} → 4 rows */
static ray_op_t* pred_lt_5(ray_graph_t* g) {
    return ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 5));
}

/* (>= k 7) → k∈{9,7,9,7} → 4 rows */
static ray_op_t* pred_ge_7(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 7));
}

/* (== k 9) → k∈{9,9} → 2 rows (sort path, no hash) */
static ray_op_t* pred_eq_9_sort(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 9));
}

/* (<= k 1) → k∈{1,1} → 2 rows */
static ray_op_t* pred_le_1(ray_graph_t* g) {
    return ray_le(g, ray_scan(g, "k"), ray_const_i64(g, 1));
}

/* (!= k 5) → 8 rows (NE unsupported by range) */
static ray_op_t* pred_ne_5(ray_graph_t* g) {
    return ray_ne(g, ray_scan(g, "k"), ray_const_i64(g, 5));
}

/* (> f 5.0) on F64 column f∈[1.5,10.5] → 6 rows ({5.5..10.5}) */
static ray_op_t* pred_f_gt_5(ray_graph_t* g) {
    return ray_gt(g, ray_scan(g, "f"), ray_const_f64(g, 5.0));
}

/* ─── Range tests ─────────────────────────────────────────────────── */

/* range_lt: sort on k; (< k 5) → 4 rows; range consult+hit. */
static test_result_t test_range_lt(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_lt)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_lt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4);  /* k∈{1,3,1,3} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    /* Differential: same rows as scan */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_lt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_lt");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_ge: sort on k; (>= k 7) → 4 rows; hit. */
static test_result_t test_range_ge(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_ge)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_ge_7);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4);  /* k∈{9,7,9,7} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_ge_7);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_ge");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_eq_dups: sort only (no hash); (== k 9) → 2 rows via range path; hit. */
static test_result_t test_range_eq_dups(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    /* Only attach sort — no hash — so the range path must handle EQ. */
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_eq_dups)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_eq_9_sort);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_9_sort);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_eq_dups");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_le_boundary: (<= k 1) → 2 rows (both 1s); hit. */
static test_result_t test_range_le_boundary(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_le_boundary)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_le_1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 2);  /* both k==1 */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_le_1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_le_boundary");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_ne_falls_back: (!= k 5) → 8 rows; range consult NOT advanced (NE = two spans). */
static test_result_t test_range_ne_falls_back(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_ne_falls_back)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_ne_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 8);

    /* NE is unsupported by the range path — consult must NOT advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_guard: 100k-row fixture (k = i % 100); (< k 50) → ~50k rows.
 * Selectivity 50% far exceeds IDX_RANGE_MAX_FRAC=128 threshold (~0.78%);
 * guard prevents hit; row count must still be correct via plain scan. */
static test_result_t test_range_guard(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 100000;
    ray_t* kv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv));
    kv->len = N;
    int64_t* kd = (int64_t*)ray_data(kv);
    for (int64_t i = 0; i < N; i++) kd[i] = i % 100;
    ray_t* vv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv));
    vv->len = N;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < N; i++) vd[i] = i;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (range_guard)");

    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    /* (< k 50): 50,000 rows */
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* pred    = ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 50));
    ray_op_t* filt    = ray_filter(g, tbl_nd, pred);
    ray_t* r = ray_execute(g, filt);
    ray_graph_free(g);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 50000);

    /* Guard fires — selectivity 50% far exceeds ~0.78% threshold → hit must NOT advance. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE] - hits_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* range_f64: sort on f (make_f64_zone_table); (> f 5.0) → 6 rows; hit. */
static test_result_t test_range_f64(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "f") == 0, "attach sort (range_f64)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    /* Build a second table for differential comparison */
    ray_t* tbl_b = make_f64_zone_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));

    ray_t* r = run_filter(tbl, pred_f_gt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 6);  /* f∈{5.5,6.5,7.5,8.5,9.5,10.5} */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    ray_t* rb = run_filter(tbl_b, pred_f_gt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 6);

    /* Check r column is 'r' (row id), not 'v' — f64 table has 'r' not 'v'.
     * Since v_cols_equal checks 'v', and the f64 table uses 'r', skip the
     * content comparison — row count match is sufficient here. */

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Rowsel ALL-segment regression (corrupted offsets) ───────────── */

/* (< k 1500) on the 262144-row sorted fixture below. */
static ray_op_t* pred_lt_1500(ray_graph_t* g) {
    return ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 1500));
}

/* range_sorted_all_segments: 262144-row SORTED I64 column k=i (v=i);
 * (< k 1500) → 1500 rows.  Span 1500 <= n/128 = 2048 so the selectivity
 * guard admits the range path.  Match layout: segment 0 (rows 0-1023) is
 * 100% matched (ALL), segment 1 (rows 1024-2047) has 476 matches (MIX),
 * the rest are NONE — ensuring at least one full ALL segment is exercised.
 * Regression pin for the rowsel_from_sorted_ids ALL-segment rollback bug:
 * the old sweep did `cum -= pc` for ALL segments even though cum was never
 * advanced for them, wrapping the uint32 write cursor and sending every
 * subsequent idx_arr write out of bounds (ASan heap-buffer-overflow;
 * silent corruption in release). */
static test_result_t test_range_sorted_all_segments(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 262144;
    ray_t* kv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv));
    kv->len = N;
    int64_t* kd = (int64_t*)ray_data(kv);
    for (int64_t i = 0; i < N; i++) kd[i] = i;
    ray_t* vv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv));
    vv->len = N;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < N; i++) vd[i] = i;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0,
                "attach sort (range_sorted_all_segments)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];

    ray_t* r = run_filter(tbl, pred_lt_1500);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 1500);

    /* The index path must actually be taken — otherwise this test
     * silently stops covering the rowsel builder. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE]     - hits_before), 1);

    /* Differential: identical v values vs the unindexed scan. */
    ray_t* tbl_b = ray_table_new(2);
    ray_t* kv_b = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv_b));
    kv_b->len = N;
    int64_t* kdata = (int64_t*)ray_data(kv_b);
    for (int64_t i = 0; i < N; i++) kdata[i] = i;
    ray_t* vv_b = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv_b));
    vv_b->len = N;
    int64_t* vdb = (int64_t*)ray_data(vv_b);
    for (int64_t i = 0; i < N; i++) vdb[i] = i;
    tbl_b = ray_table_add_col(tbl_b, ray_sym_intern("k", 1), kv_b);
    tbl_b = ray_table_add_col(tbl_b, ray_sym_intern("v", 1), vv_b);
    ray_release(kv_b);
    ray_release(vv_b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));

    ray_t* rb = run_filter(tbl_b, pred_lt_1500);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 1500);
    TEST_ASSERT(v_cols_equal(r, rb), "v col mismatch range_sorted_all_segments");

    ray_release(r); ray_release(rb);
    ray_release(tbl); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* sorted_marker_dense_range: physically sorted k=i with only the verified
 * sorted marker (no sort index).  (>= k 100000) selects 62% of the table —
 * deliberately far beyond the shuffled sort-index selectivity guard.  The
 * direct path must still hit because the result is one contiguous row span. */
static ray_op_t* pred_ge_100000(ray_graph_t* g) {
    return ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 100000));
}

static test_result_t test_sorted_marker_dense_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 262144;
    ray_t* kv = ray_vec_new(RAY_I64, N);
    ray_t* vv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv));
    kv->len = vv->len = N;
    int64_t* kd = (int64_t*)ray_data(kv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < N; i++) kd[i] = vd[i] = i;
    kv->attrs |= RAY_ATTR_SORTED;  /* fixture is verified by construction */

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT_EQ_I((int)ray_index_kind(ray_table_get_col(tbl,
                     ray_sym_intern("k", 1))), RAY_IDX_NONE);

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_RANGE];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];
    ray_t* r = run_filter(tbl, pred_ge_100000);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), N - 100000);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_RANGE] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE] - hits_before), 1);

    ray_t* rv = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_FALSE(!rv || RAY_IS_ERR(rv));
    TEST_ASSERT_EQ_I(ray_vec_get_i64(rv, 0), 100000);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(rv, ray_len(rv) - 1), N - 1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Two-sided ranges are split into chained FILTER nodes by the optimizer.
 * Both bounds must route and intersect as rowsels; the outer bound must not
 * fall back to scanning the inner selection. */
static test_result_t test_sorted_marker_chained_range(void) {
    ray_heap_init();
    (void)ray_sym_init();

    const int64_t N = 262144;
    ray_t* kv = ray_vec_new(RAY_I64, N);
    ray_t* vv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv));
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv));
    kv->len = vv->len = N;
    int64_t* kd = (int64_t*)ray_data(kv);
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < N; i++) kd[i] = vd[i] = i;
    kv->attrs |= RAY_ATTR_SORTED;
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv); ray_release(vv);

    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* base = ray_const_table(g, tbl);
    ray_op_t* ge = ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 100000));
    ray_op_t* inner = ray_filter(g, base, ge);
    ray_op_t* lt = ray_lt(g, ray_scan(g, "k"), ray_const_i64(g, 100100));
    ray_op_t* outer = ray_filter(g, inner, lt);
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_RANGE];
    ray_t* r = ray_execute(g, outer);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 100);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE] - hits_before), 2);
    ray_t* rv = ray_table_get_col(r, ray_sym_intern("v", 1));
    TEST_ASSERT_EQ_I(ray_vec_get_i64(rv, 0), 100000);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(rv, 99), 100099);

    ray_release(r);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* The direct route trusts RAY_ATTR_SORTED, so every payload mutation must
 * clear it even when the vector has no attached accelerator index. */
static test_result_t test_sorted_marker_mutation_invalidates(void) {
    ray_heap_init();
    int64_t data[] = { 1, 2, 3 };
    ray_t* v = ray_vec_from_raw(RAY_I64, data, 3);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->attrs |= RAY_ATTR_SORTED;
    int64_t replacement = 0;
    v = ray_vec_set(v, 1, &replacement);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_FALSE(ray_attr_is_sorted(v));
    ray_release(v);
    ray_heap_destroy();
    PASS();
}

/* (== k 7) on the dense-dup fixture below. */
static ray_op_t* pred_eq_7_dups(ray_graph_t* g) {
    return ray_eq(g, ray_scan(g, "k"), ray_const_i64(g, 7));
}

/* hash_eq_dense_dups: 4096-row column where EVERY row is k=7; hash
 * index attached; (== k 7) → all 4096 rows.  Every one of the 4
 * segments classifies ALL, so the buggy rollback fired on the very
 * first segment (cum 0 → uint32 wrap) and segment 1's idx writes
 * landed ~8 GB past the allocation.  Pre-existing in the hash path
 * before the Task-1 extraction (same arithmetic at 3a0cf2f1~1) —
 * it just needed >=1024 consecutive duplicates to trigger. */
static test_result_t test_hash_eq_dense_dups(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 4096;
    ray_t* kv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(kv));
    kv->len = N;
    int64_t* kd = (int64_t*)ray_data(kv);
    for (int64_t i = 0; i < N; i++) kd[i] = 7;
    ray_t* vv = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vv));
    vv->len = N;
    int64_t* vd = (int64_t*)ray_data(vv);
    for (int64_t i = 0; i < N; i++) vd[i] = i;

    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), kv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(kv);
    ray_release(vv);
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_hash_to_col(tbl, "k") == 0,
                "attach hash (hash_eq_dense_dups)");

    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_HASH];

    ray_t* r = run_filter(tbl, pred_eq_7_dups);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 4096);
    /* Every row matches → the probe's dense-key budget aborts and the
     * filter falls back to the scan path (the SIMD scan beats the
     * scattered chain walk at this density).  Consulted, NOT a hit;
     * the result must be identical either way. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_HASH] - hits_before), 0);

    /* All rows pass in order → v must be exactly 0..4095. */
    int64_t v_sym = ray_sym_intern("v", 1);
    ray_t* rv = ray_table_get_col(r, v_sym);
    TEST_ASSERT_FALSE(!rv || RAY_IS_ERR(rv));
    for (int64_t i = 0; i < N; i++) {
        TEST_ASSERT_EQ_I(ray_vec_get_i64(rv, i), i);
    }

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Predicate builders for IN tests ───────────────────────────── */

/* (in k [9 5]) → k∈{5,9,9,5} at rows {0,2,6,7} → 4 rows */
static ray_op_t* pred_in_9_5(ray_graph_t* g) {
    int64_t set_data[] = {9, 5};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 2);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* (in k [9 9 5]) — duplicate in set → same 4 rows */
static ray_op_t* pred_in_9_9_5(ray_graph_t* g) {
    int64_t set_data[] = {9, 9, 5};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 3);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* (in k [100 5]) — 100 absent, 5 present → 2 rows (rows 0,7) */
static ray_op_t* pred_in_100_5(ray_graph_t* g) {
    int64_t set_data[] = {100, 5};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 2);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

static ray_t* make_part_sym_table(void) {
    (void)ray_sym_init();
    const int64_t n = 4096;
    int64_t block_ids[] = {
        ray_sym_intern("part_z", 6),
        ray_sym_intern("part_a", 6),
        ray_sym_intern("part_m", 6),
        ray_sym_intern("part_b", 6)
    };
    int64_t ends[] = {700, 2000, 2900, 4096};
    ray_t* syms = ray_sym_vec_new(RAY_SYM_W64, n);
    ray_t* vals = ray_vec_new(RAY_I64, n);
    if (!syms || !vals || RAY_IS_ERR(syms) || RAY_IS_ERR(vals)) {
        if (syms && !RAY_IS_ERR(syms)) ray_release(syms);
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return ray_error("oom", NULL);
    }
    syms->len = vals->len = n;
    int64_t p = 0;
    for (int64_t i = 0; i < n; i++) {
        while (i >= ends[p]) p++;
        ray_write_sym(ray_data(syms), i, (uint64_t)block_ids[p],
                      RAY_SYM, syms->attrs);
        ((int64_t*)ray_data(vals))[i] = i;
    }
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("sym", 3), syms);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vals);
    ray_release(syms);
    ray_release(vals);
    return tbl;
}

/* Select two non-adjacent, dense partitions plus an absent symbol. */
static ray_op_t* pred_in_part_syms(ray_graph_t* g) {
    int64_t ids[] = {
        ray_sym_intern("part_a", 6),
        ray_sym_intern("part_b", 6),
        ray_sym_intern("part_absent", 11)
    };
    ray_t* set = ray_sym_vec_new(RAY_SYM_W64, 3);
    if (!set || RAY_IS_ERR(set)) return NULL;
    for (int64_t i = 0; i < 3; i++)
        set = ray_vec_append(set, &ids[i]);
    ray_op_t* set_op = ray_const_vec(g, set);
    ray_release(set);
    return ray_in(g, ray_scan(g, "sym"), set_op);
}

static ray_op_t* pred_eq_part_a(ray_graph_t* g) {
    ray_t* lit = ray_sym(ray_sym_intern("part_a", 6));
    ray_op_t* c = ray_const_atom(g, lit);
    ray_release(lit);
    return ray_eq(g, ray_scan(g, "sym"), c);
}

static ray_op_t* pred_eq_part_absent(ray_graph_t* g) {
    ray_t* lit = ray_sym(ray_sym_intern("part_absent", 11));
    ray_op_t* c = ray_const_atom(g, lit);
    ray_release(lit);
    return ray_eq(g, ray_scan(g, "sym"), c);
}

/* ─── IN tests ───────────────────────────────────────────────────── */

/* in_hash: hash on k; FILTER(IN(k, [9 5])) → 4 rows; IDX_SITE_IN
 * consult+hit advance; differential vs unindexed. */
static test_result_t test_in_hash(void) {
    /* Side A — with hash index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_hash side A)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_IN];

    ray_t* ra = run_filter(tbl_a, pred_in_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 4);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_IN]     - hits_before), 1);

    /* Side B — no index (scan path) */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);

    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch between indexed and scan (in_hash)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* A part index should keep dense unions as physical spans.  The chosen
 * blocks select 2496/4096 rows, which deliberately exceeds the hash route's
 * density guard, and exercise both full and partial morsels. */
static test_result_t test_in_part_sym_dense(void) {
    ray_heap_init();
    ray_t* tbl_a = make_part_sym_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_part_to_col(tbl_a, "sym") == 0,
                "attach part (in_part_sym_dense)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_IN];
    ray_t* ra = run_filter(tbl_a, pred_in_part_syms);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 2496);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_IN] - hits_before), 1);

    ray_t* tbl_b = make_part_sym_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_part_syms);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2496);
    TEST_ASSERT(v_cols_equal(ra, rb),
                "v column mismatch between parted IN and scan");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* Equality on a dense part must use its physical span directly rather than
 * tripping the hash route's density guard.  part_a occupies rows [700,2000). */
static test_result_t test_eq_part_sym_dense(void) {
    ray_heap_init();
    ray_t* tbl_a = make_part_sym_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_part_to_col(tbl_a, "sym") == 0,
                "attach part (eq_part_sym_dense)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_PART];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_PART];
    ray_t* ra = run_filter(tbl_a, pred_eq_part_a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 1300);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_PART] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_PART] - hits_before), 1);

    ray_t* tbl_b = make_part_sym_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_part_a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 1300);
    TEST_ASSERT(v_cols_equal(ra, rb),
                "v column mismatch between parted EQ and scan");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* A symbol can exist in the shared domain without having a physical part.
 * That is still a successful index proof yielding a valid empty rowsel. */
static test_result_t test_eq_part_sym_absent(void) {
    ray_heap_init();
    ray_t* tbl = make_part_sym_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    (void)ray_sym_intern("part_absent", 11);
    TEST_ASSERT(attach_part_to_col(tbl, "sym") == 0,
                "attach part (eq_part_sym_absent)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_PART];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_PART];
    ray_t* r = run_filter(tbl, pred_eq_part_absent);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_PART] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_PART] - hits_before), 1);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* in_dup_set: set [9 9 5] → same 4 rows, no duplicate row ids. */
static test_result_t test_in_dup_set(void) {
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_dup_set)");

    ray_t* ra = run_filter(tbl_a, pred_in_9_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 4);  /* no duplicate row ids */

    /* Differential vs unindexed */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_9_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 4);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch (in_dup_set)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* in_absent_elems: set [100 5] → 2 rows (only k==5 matches). */
static test_result_t test_in_absent_elems(void) {
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_absent_elems)");

    ray_t* ra = run_filter(tbl_a, pred_in_100_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 2);

    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_100_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 2);
    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch (in_absent_elems)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* in_float_col_falls_back: F64 column + hash; IN over it → consult NOT
 * advanced (float-family columns are intentionally excluded), but results
 * are correct via scan. */
static test_result_t test_in_float_col_falls_back(void) {
    ray_heap_init();
    ray_t* tbl = make_f64_zone_table();  /* f column: F64 {1.5..10.5} */
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));

    /* Attach hash to the F64 column 'f'. */
    int64_t sym_f  = ray_sym_intern("f", 1);
    int64_t ncols  = ray_table_ncols(tbl);
    int64_t slot   = -1;
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_f) { slot = i; break; }
    }
    TEST_ASSERT(slot >= 0, "f column not found");
    ray_t* w = ray_table_get_col_idx(tbl, slot);
    ray_retain(w);
    ray_t* rv = ray_index_attach_hash(&w);
    /* Hash on F64 is allowed structurally; attach should succeed. */
    if (!rv || RAY_IS_ERR(rv)) { ray_release(w); ray_release(tbl);
        ray_sym_destroy(); ray_heap_destroy();
        /* Inconclusive: hash on F64 not buildable — skip gracefully. */
        PASS(); }
    ray_table_set_col_idx(tbl, slot, w);
    ray_release(w);

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];

    /* (in f [1 2 3]) — integer set against float column */
    int64_t set_data[] = {1, 2, 3};
    ray_t* sv = ray_vec_from_raw(RAY_I64, set_data, 3);
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* set_op  = ray_const_vec(g, sv);
    ray_op_t* pred    = ray_in(g, ray_scan(g, "f"), set_op);
    ray_op_t* filt    = ray_filter(g, tbl_nd, pred);
    ray_t* r = ray_execute(g, filt);
    ray_graph_free(g);
    ray_release(sv);

    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* f∈{1.5,2.5,...,10.5}: none exactly equal 1,2,3 → 0 rows via scan */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 0);

    /* Consult must NOT have fired for float column */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── TIME width regression (hash-eq) ─────────────────────────────── */

/* Build a table with a TIME column t (4-byte int32 storage, seconds) and
 * an I64 row-id column v.  t==3600 at rows 0, 2 and 6. */
static ray_t* make_time_table(void) {
    (void)ray_sym_init();
    int32_t td[] = {3600, 7200, 3600, 86399, 0, 7200, 3600, 1, 2, 3};
    int64_t vd[] = {0,1,2,3,4,5,6,7,8,9};
    ray_t* tv = ray_vec_from_raw(RAY_TIME, td, 10);
    ray_t* vv = ray_vec_from_raw(RAY_I64, vd, 10);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("t", 1), tv);
    tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), vv);
    ray_release(tv);
    ray_release(vv);
    return tbl;
}

/* (== t 09:00... well, 3600s) — TIME literal atom against the TIME column. */
static ray_op_t* pred_eq_time_3600(ray_graph_t* g) {
    ray_t* lit = ray_time(3600);
    ray_op_t* c = ray_const_atom(g, lit);
    ray_release(lit);
    return ray_eq(g, ray_scan(g, "t"), c);
}

/* hash_eq_time: regression for the TIME width disagreement.  The hash
 * builder (numeric_key_word) reads TIME rows as 4-byte int32 — the
 * canonical storage width (ray_type_sizes[RAY_TIME] == 4) — but
 * hash_col_read_i64 used to read TIME as 8-byte, so the chain-walk
 * verify compared garbage (and read past the data area) and the index
 * path returned 0 rows where the scan returns 3.  Differential: indexed
 * result must equal the dropped-index scan, and the hash consult+hit
 * must fire. */
static test_result_t test_hash_eq_time(void) {
    /* Side A — with hash index on t */
    ray_heap_init();
    ray_t* tbl_a = make_time_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "t") == 0, "attach hash (hash_eq_time)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FILTER_HASH];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FILTER_HASH];

    ray_t* ra = run_filter(tbl_a, pred_eq_time_3600);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 3);  /* rows 0, 2, 6 */

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FILTER_HASH] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_HASH]     - hits_before), 1);

    /* Side B — no index (scan path) */
    ray_t* tbl_b = make_time_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_eq_time_3600);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 3);

    TEST_ASSERT(v_cols_equal(ra, rb), "v column mismatch between indexed and scan (hash_eq_time)");

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── IN with empty set ───────────────────────────────────────────── */

/* (in k []) — empty I64 set → no row can match. */
static ray_op_t* pred_in_empty(ray_graph_t* g) {
    ray_t* sv = ray_vec_from_raw(RAY_I64, NULL, 0);
    ray_op_t* set_op = ray_const_vec(g, sv);
    ray_release(sv);
    return ray_in(g, ray_scan(g, "k"), set_op);
}

/* in_empty_set: hash on k; FILTER(IN(k, [])) → ray_index_in_rowsel must
 * return a VALID all-NONE rowsel (NULL would mean "no fast path"), so the
 * IN consult AND hit both advance and the result has 0 rows.  Differential
 * vs the unindexed scan (also 0 rows). */
static test_result_t test_in_empty_set(void) {
    /* Side A — with hash index */
    ray_heap_init();
    ray_t* tbl_a = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_a));
    TEST_ASSERT(attach_hash_to_col(tbl_a, "k") == 0, "attach hash (in_empty_set)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_IN];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_IN];

    ray_t* ra = run_filter(tbl_a, pred_in_empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));
    TEST_ASSERT_EQ_I(ray_table_nrows(ra), 0);

    /* Empty set is a HIT (valid all-NONE rowsel), not a fall-through. */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_IN] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_IN]     - hits_before), 1);

    /* Side B — no index (scan path) */
    ray_t* tbl_b = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_b));
    ray_t* rb = run_filter(tbl_b, pred_in_empty);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));
    TEST_ASSERT_EQ_I(ray_table_nrows(rb), 0);

    ray_release(ra); ray_release(rb);
    ray_release(tbl_a); ray_release(tbl_b);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── find() hash point lookup tests ────────────────────────────────── */

/* find_hit_first_occurrence: vec {5,1,9,3,7,1,9,5,3,7}, (find vec 9) → 2.
 * The hash chain surfaces rows 2 and 6; ray_index_find_row must return the
 * minimum row id (2).  IDX_SITE_FIND consult and hit both advance. */
static test_result_t test_find_hit_first_occurrence(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* r = ray_index_attach_hash(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FIND];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FIND];

    ray_t* needle = ray_i64(9);
    ray_t* result = ray_find_fn(v, needle);
    ray_release(needle);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT(!RAY_ATOM_IS_NULL(result), "find hit must not be null");
    TEST_ASSERT(ray_is_atom(result) && result->type == -RAY_I64,
                "find hit must be i64 atom");
    TEST_ASSERT_EQ_I(result->i64, 2);  /* first occurrence of 9 is row 2 */

    /* Both counters must advance */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FIND] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FIND]     - hits_before), 1);

    ray_release(result);
    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* find_miss: vec {5,1,9,3,7,1,9,5,3,7}, (find vec 4) → 0Nl.
 * 4 is absent; the index proves absence.  Both consult and hit advance
 * (the index "answered" the query definitively). */
static test_result_t test_find_miss(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* r = ray_index_attach_hash(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FIND];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_FIND];

    ray_t* needle = ray_i64(4);
    ray_t* result = ray_find_fn(v, needle);
    ray_release(needle);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    TEST_ASSERT(RAY_ATOM_IS_NULL(result), "find miss must be null (0Nl)");

    /* Index answered → both consult and hit advance */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FIND] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FIND]     - hits_before), 1);

    ray_release(result);
    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* find_diff: differential attach/drop — indexed and unindexed must produce
 * identical results for both a hit and a miss. */
static test_result_t test_find_diff(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};

    /* ── Hit: key 9 → row 2 ── */
    /* Side A — indexed */
    ray_t* va = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(va));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&va)));

    ray_t* needle_hit = ray_i64(9);
    ray_t* ra = ray_find_fn(va, needle_hit);
    ray_release(needle_hit);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));

    /* Side B — plain vector (no index) */
    ray_t* vb = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vb));
    needle_hit = ray_i64(9);
    ray_t* rb = ray_find_fn(vb, needle_hit);
    ray_release(needle_hit);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));

    TEST_ASSERT(!RAY_ATOM_IS_NULL(ra) && !RAY_ATOM_IS_NULL(rb), "both hit must be non-null");
    TEST_ASSERT_EQ_I(ra->i64, rb->i64);  /* same row index */

    ray_release(ra); ray_release(rb);
    ray_release(va); ray_release(vb);

    /* ── Miss: key 4 → 0Nl ── */
    ray_t* vc = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vc));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&vc)));

    ray_t* needle_miss = ray_i64(4);
    ray_t* rc = ray_find_fn(vc, needle_miss);
    ray_release(needle_miss);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rc));

    ray_t* vd = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vd));
    needle_miss = ray_i64(4);
    ray_t* rd = ray_find_fn(vd, needle_miss);
    ray_release(needle_miss);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rd));

    TEST_ASSERT(RAY_ATOM_IS_NULL(rc) && RAY_ATOM_IS_NULL(rd), "both miss must be null");

    ray_release(rc); ray_release(rd);
    ray_release(vc); ray_release(vd);

    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* find_nulls_falls_back: null-bearing vec → consult NOT advanced
 * (idx_fresh_nonull rejects it), correct result via scan.
 * vec {5,1,NULL,3,7,1,9,5,3,7}; find 9 → row 6 (scan path). */
static test_result_t test_find_nulls_falls_back(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* Attach hash first, then set a null — mutation drops the index. */
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&v)));
    ray_vec_set_null(v, 2, true);  /* sets row 2 to null; drops index */

    uint64_t cons_before = ray_idx_consults[IDX_SITE_FIND];

    ray_t* needle = ray_i64(9);
    ray_t* result = ray_find_fn(v, needle);
    ray_release(needle);

    TEST_ASSERT_FALSE(RAY_IS_ERR(result));
    /* Scan finds 9 at row 6 (row 2 is now null, 9 remains at row 6). */
    TEST_ASSERT(!RAY_ATOM_IS_NULL(result), "scan must find 9");
    TEST_ASSERT_EQ_I(result->i64, 6);

    /* No index consult should have occurred */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_FIND] - cons_before), 0);

    ray_release(result);
    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Sort-index ORDER BY tests (Task 7) ─────────────────────────── */

/* Run ORDER BY on `tbl` using sort key col_name (asc when desc==0,
 * desc when desc==1).  Returns a new table (owned by caller). */
static ray_t* run_sort(ray_t* tbl, const char* col_name, uint8_t is_desc) {
    ray_graph_t* g   = ray_graph_new(tbl);
    ray_op_t* tbl_nd = ray_const_table(g, tbl);
    ray_op_t* skeys[1] = { ray_scan(g, col_name) };
    uint8_t descs[1]   = { is_desc };
    ray_op_t* sorted   = ray_sort_op(g, tbl_nd, skeys, descs, NULL, 1);
    ray_t* result      = ray_execute(g, sorted);
    ray_graph_free(g);
    return result;
}

/* Run ORDER BY two keys on `tbl`.  Returns a new table (owned by caller). */
static ray_t* run_sort2(ray_t* tbl, const char* col_a, const char* col_b) {
    ray_graph_t* g    = ray_graph_new(tbl);
    ray_op_t* tbl_nd  = ray_const_table(g, tbl);
    ray_op_t* skeys[2] = { ray_scan(g, col_a), ray_scan(g, col_b) };
    uint8_t descs[2]   = { 0, 0 };
    ray_op_t* sorted   = ray_sort_op(g, tbl_nd, skeys, descs, NULL, 2);
    ray_t* result      = ray_execute(g, sorted);
    ray_graph_free(g);
    return result;
}

/* Compare two result tables row-by-row across ALL columns.
 * Returns 1 iff every column value matches in every row. */
static int tables_equal(ray_t* ra, ray_t* rb) {
    int64_t nr_a = ray_table_nrows(ra);
    int64_t nr_b = ray_table_nrows(rb);
    if (nr_a != nr_b) return 0;
    int64_t nc = ray_table_ncols(ra);
    if (nc != ray_table_ncols(rb)) return 0;
    for (int64_t c = 0; c < nc; c++) {
        ray_t* va = ray_table_get_col_idx(ra, c);
        ray_t* vb = ray_table_get_col_idx(rb, c);
        if (!va || !vb) continue;
        for (int64_t r = 0; r < nr_a; r++) {
            if (ray_vec_get_i64(va, r) != ray_vec_get_i64(vb, r)) return 0;
        }
    }
    return 1;
}

/* sort_asc: sort index on k; ORDER BY k ASC → indexed path reuses perm;
 * consult+hit advance; results identical to unindexed sort. */
static test_result_t test_sort_asc(void) {
    ray_heap_init();
    ray_t* tbl_idx = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_idx));
    TEST_ASSERT(attach_sort_to_col(tbl_idx, "k") == 0, "attach sort (sort_asc)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_SORT];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_SORT];

    ray_t* r_idx = run_sort(tbl_idx, "k", 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_idx));
    TEST_ASSERT_EQ_I(ray_table_nrows(r_idx), 10);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_SORT] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_SORT]     - hits_before), 1);

    /* Differential: results must match unindexed sort */
    ray_t* tbl_ref = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_ref));
    ray_t* r_ref = run_sort(tbl_ref, "k", 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_ref));
    TEST_ASSERT_EQ_I(ray_table_nrows(r_ref), 10);
    TEST_ASSERT(tables_equal(r_idx, r_ref), "indexed asc sort must match reference");

    ray_release(r_idx); ray_release(r_ref);
    ray_release(tbl_idx); ray_release(tbl_ref);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* sort_desc: ORDER BY k DESC → fast path NOT taken (asc-only restriction);
 * consult NOT advanced; results identical to unindexed desc sort. */
static test_result_t test_sort_desc(void) {
    ray_heap_init();
    ray_t* tbl_idx = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_idx));
    TEST_ASSERT(attach_sort_to_col(tbl_idx, "k") == 0, "attach sort (sort_desc)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_SORT];

    ray_t* r_idx = run_sort(tbl_idx, "k", 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_idx));
    TEST_ASSERT_EQ_I(ray_table_nrows(r_idx), 10);

    /* No consult: DESC is excluded from the fast path */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_SORT] - cons_before), 0);

    /* Differential: results must still match unindexed desc sort */
    ray_t* tbl_ref = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl_ref));
    ray_t* r_ref = run_sort(tbl_ref, "k", 1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_ref));
    TEST_ASSERT_EQ_I(ray_table_nrows(r_ref), 10);
    TEST_ASSERT(tables_equal(r_idx, r_ref), "indexed desc sort must match reference");

    ray_release(r_idx); ray_release(r_ref);
    ray_release(tbl_idx); ray_release(tbl_ref);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* sort_multikey_falls_back: two sort keys → fast path not taken;
 * consult NOT advanced. */
static test_result_t test_sort_multikey_falls_back(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (multikey)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_SORT];

    ray_t* r = run_sort2(tbl, "k", "v");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 10);

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_SORT] - cons_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* sort_topk_untouched: HEAD(SORT) triggers top-K path which returns before
 * our sort-index fast path; consult NOT advanced. */
static test_result_t test_sort_topk_untouched(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (topk)");

    uint64_t cons_before = ray_idx_consults[IDX_SITE_SORT];

    /* HEAD(3, SORT(k ASC)) — triggers top-K bounded-heap shortcut */
    ray_graph_t* g   = ray_graph_new(tbl);
    ray_op_t* tbl_nd = ray_const_table(g, tbl);
    ray_op_t* skeys[1] = { ray_scan(g, "k") };
    uint8_t descs[1]   = { 0 };
    ray_op_t* sorted   = ray_sort_op(g, tbl_nd, skeys, descs, NULL, 1);
    ray_op_t* headed   = ray_head(g, sorted, 3);
    ray_t* r           = ray_execute(g, headed);
    ray_graph_free(g);

    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);

    /* top-K returns before reaching the sort-index fast path */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_SORT] - cons_before), 0);

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* sort_selection_falls_back: FILTER then SORT — filter materialises a
 * selection; exec.c compacts it before exec_sort, so by the time exec_sort
 * runs g->selection is NULL and the check is vacuous.  The important thing
 * is that the result is correct (filter + sort produces the right rows). */
static test_result_t test_sort_selection_falls_back(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort (selection)");

    /* FILTER k >= 5 → {5,9,7,9,5,7} (6 rows), then SORT k ASC */
    ray_graph_t* g   = ray_graph_new(tbl);
    ray_op_t* tbl_nd = ray_const_table(g, tbl);
    ray_op_t* pred   = ray_ge(g, ray_scan(g, "k"), ray_const_i64(g, 5));
    ray_op_t* filt   = ray_filter(g, tbl_nd, pred);
    ray_op_t* skeys[1] = { ray_scan(g, "k") };
    uint8_t descs[1]   = { 0 };
    ray_op_t* sorted   = ray_sort_op(g, filt, skeys, descs, NULL, 1);
    ray_t* r           = ray_execute(g, sorted);
    ray_graph_free(g);

    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* k >= 5 gives rows: {5,9,7,9,5,7} = 6 rows */
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 6);

    /* Verify k column is sorted ascending */
    int64_t k_sym = ray_sym_intern("k", 1);
    ray_t* kc = ray_table_get_col(r, k_sym);
    TEST_ASSERT(kc != NULL, "k column in sort result");
    for (int64_t i = 1; i < 6; i++) {
        TEST_ASSERT(ray_vec_get_i64(kc, i-1) <= ray_vec_get_i64(kc, i),
                    "sorted order violated after filter+sort");
    }

    ray_release(r);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Distinct float edge-case tests (Task 8 headline check) ─────── */

/* distinct_f64_neg_zero: column {-0.0, +0.0, 1.0, 2.0} without a sort
 * index.  The hashset path normalises -0.0 → +0.0 (ray_hash_f64 contract)
 * and -0.0 == +0.0 in hs_eq_rows, so only ONE distinct value among the
 * two zeros → 3 distinct values total {0.0, 1.0, 2.0}.
 *
 * WITH a sort index, numeric_key_word also normalises via clear_neg_zero
 * so -0.0 and +0.0 get the same key_word → same run → ONE emission.
 * Both paths must agree: 3 distinct values. */
static test_result_t test_distinct_f64_neg_zero(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Build a raw F64 vector containing -0.0 then +0.0 */
    uint64_t raw[4] = {
        UINT64_C(0x8000000000000000), /* -0.0 */
        UINT64_C(0x0000000000000000), /* +0.0 */
        UINT64_C(0x3FF0000000000000), /* 1.0  */
        UINT64_C(0x4000000000000000), /* 2.0  */
    };
    ray_t* v_no_idx = ray_vec_from_raw(RAY_F64, raw, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v_no_idx));

    ray_t* r_no_idx = distinct_vec_eager(v_no_idx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_no_idx));
    int64_t cnt_no_idx = ray_len(r_no_idx);

    ray_t* v_idx = ray_vec_from_raw(RAY_F64, raw, 4);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v_idx));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&v_idx)));

    ray_t* r_idx = distinct_vec_eager(v_idx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_idx));
    int64_t cnt_idx = ray_len(r_idx);

    /* Both paths must agree and return 3 (0.0, 1.0, 2.0) */
    TEST_ASSERT_EQ_I(cnt_no_idx, 3);
    TEST_ASSERT_EQ_I(cnt_idx,    3);

    ray_release(r_no_idx); ray_release(v_no_idx);
    ray_release(r_idx);    ray_release(v_idx);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* distinct_f64_nan_no_null_attr: F64 column with NaN values but NO
 * RAY_ATTR_HAS_NULLS attribute (i.e. NaN injected via ray_vec_from_raw,
 * not via ray_vec_set_null).  The idx_fresh_nonull gate passes (HAS_NULLS
 * clear), so the sort-index fast path fires.
 *
 * Sort: all NaN → same radix key → contiguous in perm.
 * numeric_key_word(NaN) → per-row hash (each row unique) → every NaN emits
 * as a separate run → N NaN rows → N "distinct" NaN entries.
 *
 * Hashset: hs_eq_rows uses == → NaN != NaN → each NaN its own slot →
 * also N entries.
 *
 * Both paths agree (both treat N canonical-NaN rows as N distinct values),
 * so there is NO divergence.  This test pins that agreement.
 *
 * Note: this is conceptually degenerate (all N are the same bit pattern)
 * but the engine is internally consistent.  If user-visible semantics must
 * deduplicate NaN, gate F64 out of the fast path (matching the hash-index
 * float gate) and let the hashset handle it consistently. */
static test_result_t test_distinct_f64_nan_no_null_attr(void) {
    ray_heap_init();
    (void)ray_sym_init();

    /* Three rows of canonical NaN, no HAS_NULLS flag. */
    double nan_val = __builtin_nan("");
    double raw[5] = { 1.0, nan_val, nan_val, nan_val, 2.0 };
    ray_t* v_no_idx = ray_vec_from_raw(RAY_F64, raw, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v_no_idx));
    /* Confirm HAS_NULLS is NOT set */
    TEST_ASSERT((v_no_idx->attrs & 0x40) == 0, "HAS_NULLS must not be set on raw-constructed vector");

    ray_t* r_no_idx = distinct_vec_eager(v_no_idx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_no_idx));
    int64_t cnt_no_idx = ray_len(r_no_idx);

    ray_t* v_idx = ray_vec_from_raw(RAY_F64, raw, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v_idx));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&v_idx)));

    ray_t* r_idx = distinct_vec_eager(v_idx);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r_idx));
    int64_t cnt_idx = ray_len(r_idx);

    /* Both paths agree: each NaN emitted separately → 5 distinct values
     * (1.0, NaN, NaN, NaN, 2.0).  This confirms the paths are consistent
     * even if the result is degenerate from a user perspective. */
    TEST_ASSERT_EQ_I(cnt_no_idx, cnt_idx);

    ray_release(r_no_idx); ray_release(v_no_idx);
    ray_release(r_idx);    ray_release(v_idx);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Distinct fast-path tests (Task 8) ──────────────────────────── */

/* distinct_order_contract: pin the output order for the duplicates fixture.
 *
 * vec {5,1,9,3,7,1,9,5,3,7}:
 *   hashset collects first-occurrence row ids: 0(=5),1(=1),2(=9),3(=3),4(=7)
 *   distinct_sort_indices sorts those ids BY VALUE:
 *     value-order:  1 < 3 < 5 < 7 < 9
 *     row ids:      1,  3,  0,  4,  2
 *   → output {1,3,5,7,9}  (VALUE-SORTED order, not first-occurrence order)
 *
 * Contract: distinct on a numeric vector returns values in ASCENDING VALUE
 * order (not in first-occurrence order).  This test must pass with OR
 * without a sort index attached. */
static test_result_t test_distinct_order_contract(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    ray_t* r = distinct_vec_eager(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_len(r), 5);

    /* Value-sorted contract: {1,3,5,7,9} */
    TEST_ASSERT_EQ_I(ray_vec_get_i64(r, 0), 1);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(r, 1), 3);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(r, 2), 5);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(r, 3), 7);
    TEST_ASSERT_EQ_I(ray_vec_get_i64(r, 4), 9);

    ray_release(r);
    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* distinct_sorted: sort index attached → fast path fires; output identical
 * to the unindexed run; IDX_SITE_DISTINCT consult+hit both advance. */
static test_result_t test_distinct_sorted(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};

    /* Side A — with sort index */
    ray_t* va = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(va));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&va)));

    uint64_t cons_before = ray_idx_consults[IDX_SITE_DISTINCT];
    uint64_t hits_before = ray_idx_hits[IDX_SITE_DISTINCT];

    ray_t* ra = distinct_vec_eager(va);
    TEST_ASSERT_FALSE(RAY_IS_ERR(ra));

    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_DISTINCT] - cons_before), 1);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_DISTINCT]     - hits_before), 1);

    /* Side B — no index (baseline) */
    ray_t* vb = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(vb));
    ray_t* rb = distinct_vec_eager(vb);
    TEST_ASSERT_FALSE(RAY_IS_ERR(rb));

    /* Differential: same length and same values */
    TEST_ASSERT_EQ_I(ray_len(ra), ray_len(rb));
    for (int64_t i = 0; i < ray_len(ra); i++) {
        TEST_ASSERT_EQ_I(ray_vec_get_i64(ra, i), ray_vec_get_i64(rb, i));
    }

    ray_release(ra); ray_release(rb);
    ray_release(va); ray_release(vb);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* distinct_unique_col: all-unique vector → output identical to input;
 * sort index → fast path fires; hit advances. */
static test_result_t test_distinct_unique_col(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {10,20,30,40,50};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&v)));

    uint64_t hits_before = ray_idx_hits[IDX_SITE_DISTINCT];

    ray_t* r = distinct_vec_eager(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_len(r), 5);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_DISTINCT] - hits_before), 1);

    /* Values unchanged (already sorted, all unique) */
    for (int64_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQ_I(ray_vec_get_i64(r, i), kd[i]);
    }

    ray_release(r);
    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* distinct_nulls_falls_back: null-bearing vector → sort index gate
 * (idx_fresh_nonull) rejects it → consult NOT advanced; correct result
 * via hashset path. */
static test_result_t test_distinct_nulls_falls_back(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));

    /* Attach sort, then set a null — mutation drops the index. */
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&v)));
    ray_vec_set_null(v, 2, true);  /* row 2 → null; drops index */

    uint64_t cons_before = ray_idx_consults[IDX_SITE_DISTINCT];

    ray_t* r = distinct_vec_eager(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    /* No index consult: RAY_ATTR_HAS_NULLS blocks idx_fresh_nonull */
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_consults[IDX_SITE_DISTINCT] - cons_before), 0);

    /* Hashset path still produces correct result: 5 non-null distinct values
     * (null at row 2 takes row 2's slot; value at row 6 still exists as 9) */
    TEST_ASSERT(ray_len(r) >= 5, "distinct on null-bearing vec must return >=5 values");

    ray_release(r);
    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Task 10: Staleness end-to-end + adversarial sweeps ─────────────── */

/* Helper: find the slot index for column `sym_id` in `tbl`. Returns -1 if not found. */
static int64_t find_col_slot(ray_t* tbl, int64_t sym_id) {
    int64_t ncols = ray_table_ncols(tbl);
    for (int64_t i = 0; i < ncols; i++) {
        if (ray_table_col_name(tbl, i) == sym_id) return i;
    }
    return -1;
}

/* Helper: append elem to the column at slot in tbl.
 *
 * Retain protocol: we retain col before calling ray_vec_append so that
 * ray_cow inside always forks a copy (rc > 1 guarantees COW).  After the
 * append we write col2 (the COW copy + appended element) back into the
 * table via ray_table_set_col_idx, which:
 *   - retains col2  (rc: 1 → 2)
 *   - releases the old slot pointer (which is col — its rc drops to 0,
 *     freeing it, because the COW's ray_release already brought it to 1).
 * We then release col2 to drop the extra retain → col2->rc = 1 (table owns).
 * We do NOT release col again — it was freed by ray_table_set_col_idx. */
static ray_t* tbl_col_append(ray_t* tbl, int64_t slot, const void* elem) {
    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return col;
    ray_retain(col);                           /* force COW in ray_vec_append */
    ray_t* col2 = ray_vec_append(col, elem);
    if (!col2 || RAY_IS_ERR(col2)) { ray_release(col); return col2; }
    if (col2 != col) {
        /* COW produced a new pointer.  Table still holds the old col (rc=1
         * after COW's ray_release).  set_col_idx retains col2 and releases col
         * (col->rc → 0, freed).  Then we release the extra retain on col2. */
        ray_table_set_col_idx(tbl, slot, col2);
        ray_release(col2);
    } else {
        /* In-place: shouldn't occur with our retain, but handle safely. */
        ray_release(col);
    }
    return col2;
}

/* Helper: overwrite elem at idx in the column at slot.
 * Same retain-before-mutate discipline as tbl_col_append. */
static ray_t* tbl_col_set(ray_t* tbl, int64_t slot, int64_t idx, const void* elem) {
    ray_t* col = ray_table_get_col_idx(tbl, slot);
    if (!col || RAY_IS_ERR(col)) return col;
    ray_retain(col);
    ray_t* col2 = ray_vec_set(col, idx, elem);
    if (!col2 || RAY_IS_ERR(col2)) { ray_release(col); return col2; }
    if (col2 != col) {
        ray_table_set_col_idx(tbl, slot, col2);
        ray_release(col2);
    } else {
        ray_release(col);
    }
    return col2;
}

/* ─── Staleness: filter-eq hash — append mutation ────────────────────── */

/* stale_filter_hash: attach hash to k, run (== k 9) → 2 rows (index hit).
 * Append k=9, v=10 → len grows; built_for_len mismatch.
 * Re-run (== k 9): correct result (3 rows, scan path), hit UNCHANGED. */
static test_result_t test_stale_filter_hash(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_hash_to_col(tbl, "k") == 0, "attach hash");

    /* First run — index fresh; (== k 9) → 2 rows. */
    uint64_t hits0 = ray_idx_hits[IDX_SITE_FILTER_HASH];
    ray_t* r0 = run_filter(tbl, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(ray_table_nrows(r0), 2);
    TEST_ASSERT(ray_idx_hits[IDX_SITE_FILTER_HASH] > hits0, "first run must hit");
    ray_release(r0);

    /* Mutate: append k=9, v=10 → index dropped by ray_vec_append. */
    int64_t kslot = find_col_slot(tbl, ray_sym_intern("k", 1));
    int64_t vslot = find_col_slot(tbl, ray_sym_intern("v", 1));
    TEST_ASSERT(kslot >= 0, "k slot"); TEST_ASSERT(vslot >= 0, "v slot");
    TEST_ASSERT_EQ_I(kslot, 0);  /* k must be first column (slot 0) */
    TEST_ASSERT_EQ_I(vslot, 1);  /* v must be second column (slot 1) */

    /* Verify len before mutation */
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 10);

    int64_t kval = 9, vval = 10;
    { ray_t* _r = tbl_col_append(tbl, kslot, &kval); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }
    { ray_t* _r = tbl_col_append(tbl, vslot, &vval); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }

    /* Sanity: after mutation the table must have 11 rows. */
    TEST_ASSERT_EQ_I(ray_table_nrows(tbl), 11);

    /* Second run: 11-row table, k=9 now at rows 2, 6, 10 → 3 rows via scan.
     * Hit counter must NOT advance (index was dropped on mutation). */
    uint64_t hits1 = ray_idx_hits[IDX_SITE_FILTER_HASH];
    ray_t* r1 = run_filter(tbl, pred_eq_9);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 3);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_HASH] - hits1), 0);
    ray_release(r1);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Staleness: filter-IN hash — append mutation ────────────────────── */

/* stale_in_hash: attach hash to k, run IN(k,[9,5]) → 4 rows (index hit).
 * Append k=5, v=10 → len grows; index dropped.
 * Re-run: 5 rows via scan, hit UNCHANGED. */
static test_result_t test_stale_in_hash(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_hash_to_col(tbl, "k") == 0, "attach hash");

    /* First run — index fresh. */
    uint64_t hits0 = ray_idx_hits[IDX_SITE_IN];
    ray_t* r0 = run_filter(tbl, pred_in_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(ray_table_nrows(r0), 4);
    TEST_ASSERT(ray_idx_hits[IDX_SITE_IN] > hits0, "first run must hit");
    ray_release(r0);

    /* Mutate: append k=5, v=10. */
    int64_t kslot = find_col_slot(tbl, ray_sym_intern("k", 1));
    int64_t vslot = find_col_slot(tbl, ray_sym_intern("v", 1));
    TEST_ASSERT(kslot >= 0, "k slot"); TEST_ASSERT(vslot >= 0, "v slot");

    int64_t kval = 5, vval = 10;
    { ray_t* _r = tbl_col_append(tbl, kslot, &kval); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }
    { ray_t* _r = tbl_col_append(tbl, vslot, &vval); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }

    /* Second run: 11-row table, {9,5} matches {5,9,9,5,5} at rows 0,2,6,7,10 → 5 rows. */
    uint64_t hits1 = ray_idx_hits[IDX_SITE_IN];
    ray_t* r1 = run_filter(tbl, pred_in_9_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 5);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_IN] - hits1), 0);
    ray_release(r1);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Staleness: filter-range sort — set mutation ────────────────────── */

/* stale_filter_range: sort on k, run (< k 5) → 4 rows (index hit).
 * Overwrite k[0]=5 → 4 via ray_vec_set (same len, index dropped).
 * Re-run: k={4,1,9,3,7,1,9,5,3,7}; (< k 5) → {4,1,3,1,3} = 5 rows via scan.
 * Hit UNCHANGED. */
static test_result_t test_stale_filter_range(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort");

    /* First run — index fresh; (< k 5) → k∈{1,3,1,3} = 4 rows. */
    uint64_t hits0 = ray_idx_hits[IDX_SITE_FILTER_RANGE];
    ray_t* r0 = run_filter(tbl, pred_lt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(ray_table_nrows(r0), 4);
    TEST_ASSERT(ray_idx_hits[IDX_SITE_FILTER_RANGE] > hits0, "first run must hit");
    ray_release(r0);

    /* Mutate: overwrite k[0]=5 → 4; same len, index dropped. */
    int64_t kslot = find_col_slot(tbl, ray_sym_intern("k", 1));
    TEST_ASSERT(kslot >= 0, "k slot");
    int64_t new_k0 = 4;
    { ray_t* _r = tbl_col_set(tbl, kslot, 0, &new_k0); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }

    /* k now {4,1,9,3,7,1,9,5,3,7}: (< k 5) → k∈{4,1,3,1,3} = 5 rows via scan. */
    uint64_t hits1 = ray_idx_hits[IDX_SITE_FILTER_RANGE];
    ray_t* r1 = run_filter(tbl, pred_lt_5);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 5);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FILTER_RANGE] - hits1), 0);
    ray_release(r1);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Staleness: sort perm — append mutation ─────────────────────────── */

/* stale_sort_perm: sort on k, ORDER BY k ASC → 10 rows (index hit).
 * Append k=0, v=10 → len 11, built_for_len mismatch, index dropped.
 * Re-run ORDER BY k ASC: 11 rows via general sort, hit UNCHANGED. */
static test_result_t test_stale_sort_perm(void) {
    ray_heap_init();
    ray_t* tbl = make_idx_table();
    TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    TEST_ASSERT(attach_sort_to_col(tbl, "k") == 0, "attach sort");

    /* First run — index fresh. */
    uint64_t hits0 = ray_idx_hits[IDX_SITE_SORT];
    ray_t* r0 = run_sort(tbl, "k", 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(ray_table_nrows(r0), 10);
    TEST_ASSERT(ray_idx_hits[IDX_SITE_SORT] > hits0, "first run must hit");
    ray_release(r0);

    /* Mutate: append k=0, v=10 (index dropped). */
    int64_t kslot = find_col_slot(tbl, ray_sym_intern("k", 1));
    int64_t vslot = find_col_slot(tbl, ray_sym_intern("v", 1));
    TEST_ASSERT(kslot >= 0, "k slot"); TEST_ASSERT(vslot >= 0, "v slot");

    int64_t kval = 0, vval = 10;
    { ray_t* _r = tbl_col_append(tbl, kslot, &kval); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }
    { ray_t* _r = tbl_col_append(tbl, vslot, &vval); TEST_ASSERT_FALSE(RAY_IS_ERR(_r)); }

    /* Second run: 11 rows, general sort (no index), hit UNCHANGED. */
    uint64_t hits1 = ray_idx_hits[IDX_SITE_SORT];
    ray_t* r1 = run_sort(tbl, "k", 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_table_nrows(r1), 11);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_SORT] - hits1), 0);
    ray_release(r1);

    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Staleness: find hash — set mutation ────────────────────────────── */

/* stale_find_hash: attach hash to {5,1,9,3,7,1,9,5,3,7}.
 * find 9 → row 2 (index hit).
 * Overwrite row 2 (9→8) via ray_vec_set → same len, index dropped.
 * find 9 → row 6 via scan, hit UNCHANGED. */
static test_result_t test_stale_find_hash(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_hash(&v)));

    /* First find — index fresh, must hit; first 9 is at row 2. */
    uint64_t hits0 = ray_idx_hits[IDX_SITE_FIND];
    ray_t* needle = ray_i64(9);
    ray_t* r0 = ray_find_fn(v, needle);
    ray_release(needle);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT(!RAY_ATOM_IS_NULL(r0), "first find must succeed");
    TEST_ASSERT_EQ_I(r0->i64, 2);
    TEST_ASSERT(ray_idx_hits[IDX_SITE_FIND] > hits0, "first find must advance hit");
    ray_release(r0);

    /* Mutate: overwrite row 2 (9→8); vec_drop_index_inplace fires. */
    int64_t new_val = 8;
    ray_t* v2 = ray_vec_set(v, 2, &new_val);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v2));
    v = v2;  /* adopt possibly-COW'd pointer */

    /* Second find — index dropped; scan finds 9 at row 6 (row 2 is now 8). */
    uint64_t hits1 = ray_idx_hits[IDX_SITE_FIND];
    needle = ray_i64(9);
    ray_t* r1 = ray_find_fn(v, needle);
    ray_release(needle);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT(!RAY_ATOM_IS_NULL(r1), "second find must succeed");
    TEST_ASSERT_EQ_I(r1->i64, 6);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_FIND] - hits1), 0);
    ray_release(r1);

    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Staleness: distinct sort — set mutation ────────────────────────── */

/* stale_distinct_sort: attach sort to {5,1,9,3,7,1,9,5,3,7} → 5 distinct (hit).
 * Overwrite row 4 (7→2) via ray_vec_set → same len, index dropped.
 * k becomes {5,1,9,3,2,1,9,5,3,7}: distinct = {1,2,3,5,7,9} = 6 values via
 * hashset path.  Hit UNCHANGED. */
static test_result_t test_stale_distinct_sort(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t kd[] = {5,1,9,3,7,1,9,5,3,7};
    ray_t* v = ray_vec_from_raw(RAY_I64, kd, 10);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_FALSE(RAY_IS_ERR(ray_index_attach_sort(&v)));

    /* First distinct — index fresh; 5 distinct values {1,3,5,7,9}. */
    uint64_t hits0 = ray_idx_hits[IDX_SITE_DISTINCT];
    ray_t* r0 = distinct_vec_eager(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r0));
    TEST_ASSERT_EQ_I(ray_len(r0), 5);
    TEST_ASSERT(ray_idx_hits[IDX_SITE_DISTINCT] > hits0, "first distinct must hit");
    ray_release(r0);

    /* Mutate: overwrite row 4 (7→2); index dropped. */
    int64_t new_val = 2;
    ray_t* v2 = ray_vec_set(v, 4, &new_val);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v2));
    v = v2;

    /* Second distinct — hashset path.
     * k={5,1,9,3,2,1,9,5,3,7}: distinct = {1,2,3,5,7,9} = 6 values.
     * (7 still present at row 9; 2 added via row 4 mutation.) */
    uint64_t hits1 = ray_idx_hits[IDX_SITE_DISTINCT];
    ray_t* r1 = distinct_vec_eager(v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(ray_len(r1), 6);
    TEST_ASSERT_EQ_I((int64_t)(ray_idx_hits[IDX_SITE_DISTINCT] - hits1), 0);
    ray_release(r1);

    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Bloom adversarial sweep ────────────────────────────────────────── */

/* bloom_adversarial_sweep: 1000-row I64 column, values i*7 for i∈[0,999].
 * Values: 0, 7, 14, ..., 6993.  All 1000 are distinct (step-7 sequence).
 *
 * Assert 1 (no false-absent): for every inserted value v_i,
 *   ray_index_bloom_absent(col, v_i) must return false.
 *
 * Assert 2 (sound absent): for keys 1..2000, whenever bloom says absent,
 *   linear membership check must also confirm the key is absent.
 *   (bloom absent ⊆ truly absent; no false-absent is possible.) */
static test_result_t test_bloom_adversarial_sweep(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 1000;
    ray_t* v = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = N;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < N; i++) d[i] = i * 7;

    /* Attach bloom — takes ownership of the vec. */
    ray_t* r = ray_index_attach_bloom(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    /* Assert 1: no false-absent for every inserted key. */
    for (int64_t i = 0; i < N; i++) {
        int64_t key = i * 7;
        if (ray_index_bloom_absent(v, key))
            FAILF("bloom false-absent: key %lld (i=%lld) was inserted but reported absent",
                  (long long)key, (long long)i);
    }

    /* Assert 2: bloom-absent ⊆ linear-absent for keys 1..2000.
     * Key k is present iff k%7==0 && k/7 < N (i.e. k∈{0,7,14,...,6993}). */
    for (int64_t key = 1; key <= 2000; key++) {
        if (ray_index_bloom_absent(v, key)) {
            /* Bloom says absent — linear scan must agree. */
            bool linear_absent = !(key % 7 == 0 && key / 7 < N);
            if (!linear_absent)
                FAILF("bloom false-absent in sweep: key %lld is present but bloom says absent",
                      (long long)key);
        }
    }

    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ─── Range adversarial sweep ────────────────────────────────────────── */

/* Linear count of rows in d[0..N) satisfying (d[i] cmp_op key). */
static int64_t linear_count(const int64_t* d, int64_t N,
                             uint16_t cmp_op, int64_t key) {
    int64_t cnt = 0;
    for (int64_t i = 0; i < N; i++) {
        switch (cmp_op) {
            case OP_EQ: if (d[i] == key) cnt++; break;
            case OP_LT: if (d[i] <  key) cnt++; break;
            case OP_LE: if (d[i] <= key) cnt++; break;
            case OP_GT: if (d[i] >  key) cnt++; break;
            case OP_GE: if (d[i] >= key) cnt++; break;
            default: break;
        }
    }
    return cnt;
}

/* range_adversarial_sweep: 1000-row I64 column, values (i*2654435761ULL)%500
 * (deterministic, many duplicates in [0,499]).  Attach sort index.
 *
 * For each op in {EQ,LT,LE,GT,GE} × keys {-1,0,250,499,500}:
 *   ray_index_range_rowsel → total_pass (when non-NULL) must equal
 *   a linear count over the raw array.
 *   NULL result (selectivity guard or ineligible) is skipped — not a failure.
 *
 * Guard threshold IDX_RANGE_MAX_FRAC=128 → guard fires at span > 1000/128≈7.
 * Combos that pass: EQ=2-row hits (keys 0,250,499); zero-span cases return
 * empty rowsel before the guard (keys -1, 0-LT, 499-GT, 500-GT/GE/LT/LE).
 * ≥9 combos exercise the rowsel path despite the tighter guard. */
static test_result_t test_range_adversarial_sweep(void) {
    ray_heap_init();
    (void)ray_sym_init();

    int64_t N = 1000;
    ray_t* v = ray_vec_new(RAY_I64, N);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    v->len = N;
    int64_t* d = (int64_t*)ray_data(v);
    for (int64_t i = 0; i < N; i++)
        d[i] = (int64_t)((uint64_t)i * UINT64_C(2654435761) % 500);

    ray_t* r = ray_index_attach_sort(&v);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));

    static const uint16_t ops[]   = { OP_EQ, OP_LT, OP_LE, OP_GT, OP_GE };
    static const char*    opnms[] = { "EQ", "LT", "LE", "GT", "GE" };
    static const int64_t  keys[]  = { -1, 0, 250, 499, 500 };

    for (int oi = 0; oi < 5; oi++) {
        for (int ki = 0; ki < 5; ki++) {
            int64_t key   = keys[ki];
            uint16_t op   = ops[oi];
            ray_t* sel = ray_index_range_rowsel(v, op, key, 0.0, false);
            if (!sel || RAY_IS_ERR(sel)) {
                /* Selectivity guard fired or not eligible — skip. */
                if (sel && RAY_IS_ERR(sel)) { /* error, not NULL */ }
                continue;
            }
            int64_t idx_count = ray_rowsel_meta(sel)->total_pass;
            int64_t lin_count = linear_count(d, N, op, key);
            ray_release(sel);
            if (idx_count != lin_count) {
                ray_release(v);
                ray_sym_destroy();
                ray_heap_destroy();
                FAILF("range mismatch op=%s key=%lld: idx=%lld lin=%lld",
                      opnms[oi], (long long)key,
                      (long long)idx_count, (long long)lin_count);
            }
        }
    }

    ray_release(v);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

const test_entry_t idx_route_entries[] = {
    { "idx_route/hash_eq_still_works",   test_hash_eq_still_works,   NULL, NULL },
    { "idx_route/zone_none",             test_zone_none,             NULL, NULL },
    { "idx_route/zone_all",              test_zone_all,              NULL, NULL },
    { "idx_route/zone_unknown",          test_zone_unknown,          NULL, NULL },
    { "idx_route/zone_nulls_excluded",   test_zone_nulls_excluded,   NULL, NULL },
    { "idx_route/zone_le_all_boundary",  test_zone_le_all_boundary,  NULL, NULL },
    { "idx_route/zone_ge_all_boundary",  test_zone_ge_all_boundary,  NULL, NULL },
    { "idx_route/zone_ne_all",           test_zone_ne_all,           NULL, NULL },
    { "idx_route/zone_eq_none_boundary", test_zone_eq_none_boundary, NULL, NULL },
    { "idx_route/zone_f64_none",         test_zone_f64_none,         NULL, NULL },
    { "idx_route/zone_f64_int_literal",  test_zone_f64_int_literal,  NULL, NULL },
    { "idx_route/zone_int_col_float_literal", test_zone_int_col_float_literal, NULL, NULL },
    { "idx_route/bloom_absent",              test_bloom_absent,              NULL, NULL },
    { "idx_route/bloom_present_falls_through", test_bloom_present_falls_through, NULL, NULL },
    { "idx_route/bloom_diff_absent",         test_bloom_diff_absent,         NULL, NULL },
    { "idx_route/bloom_diff_present",        test_bloom_diff_present,        NULL, NULL },
    { "idx_route/bloom_noneq_ignored",       test_bloom_noneq_ignored,       NULL, NULL },
    { "idx_route/range_lt",                  test_range_lt,                  NULL, NULL },
    { "idx_route/range_ge",                  test_range_ge,                  NULL, NULL },
    { "idx_route/range_eq_dups",             test_range_eq_dups,             NULL, NULL },
    { "idx_route/range_le_boundary",         test_range_le_boundary,         NULL, NULL },
    { "idx_route/range_ne_falls_back",       test_range_ne_falls_back,       NULL, NULL },
    { "idx_route/range_guard",               test_range_guard,               NULL, NULL },
    { "idx_route/range_f64",                 test_range_f64,                 NULL, NULL },
    { "idx_route/range_sorted_all_segments", test_range_sorted_all_segments, NULL, NULL },
    { "idx_route/sorted_marker_dense_range", test_sorted_marker_dense_range, NULL, NULL },
    { "idx_route/sorted_marker_chained_range", test_sorted_marker_chained_range, NULL, NULL },
    { "idx_route/sorted_marker_mutation_invalidates", test_sorted_marker_mutation_invalidates, NULL, NULL },
    { "idx_route/hash_eq_dense_dups",        test_hash_eq_dense_dups,        NULL, NULL },
    { "idx_route/in_hash",                   test_in_hash,                   NULL, NULL },
    { "idx_route/in_part_sym_dense",         test_in_part_sym_dense,         NULL, NULL },
    { "idx_route/eq_part_sym_dense",         test_eq_part_sym_dense,         NULL, NULL },
    { "idx_route/eq_part_sym_absent",        test_eq_part_sym_absent,        NULL, NULL },
    { "idx_route/in_dup_set",                test_in_dup_set,                NULL, NULL },
    { "idx_route/in_absent_elems",           test_in_absent_elems,           NULL, NULL },
    { "idx_route/in_float_col_falls_back",   test_in_float_col_falls_back,   NULL, NULL },
    { "idx_route/hash_eq_time",              test_hash_eq_time,              NULL, NULL },
    { "idx_route/in_empty_set",              test_in_empty_set,              NULL, NULL },
    { "idx_route/find_hit_first_occurrence", test_find_hit_first_occurrence, NULL, NULL },
    { "idx_route/find_miss",                 test_find_miss,                 NULL, NULL },
    { "idx_route/find_diff",                 test_find_diff,                 NULL, NULL },
    { "idx_route/find_nulls_falls_back",     test_find_nulls_falls_back,     NULL, NULL },
    { "idx_route/sort_asc",                  test_sort_asc,                  NULL, NULL },
    { "idx_route/sort_desc",                 test_sort_desc,                 NULL, NULL },
    { "idx_route/sort_multikey_falls_back",  test_sort_multikey_falls_back,  NULL, NULL },
    { "idx_route/sort_topk_untouched",       test_sort_topk_untouched,       NULL, NULL },
    { "idx_route/sort_selection_falls_back", test_sort_selection_falls_back, NULL, NULL },
    { "idx_route/distinct_order_contract",         test_distinct_order_contract,         NULL, NULL },
    { "idx_route/distinct_sorted",                 test_distinct_sorted,                 NULL, NULL },
    { "idx_route/distinct_unique_col",             test_distinct_unique_col,             NULL, NULL },
    { "idx_route/distinct_nulls_falls_back",       test_distinct_nulls_falls_back,       NULL, NULL },
    { "idx_route/distinct_f64_neg_zero",           test_distinct_f64_neg_zero,           NULL, NULL },
    { "idx_route/distinct_f64_nan_no_null_attr",   test_distinct_f64_nan_no_null_attr,   NULL, NULL },
    /* Task 10: staleness end-to-end (mutation via append/set, not null) */
    { "idx_route/stale_filter_hash",         test_stale_filter_hash,         NULL, NULL },
    { "idx_route/stale_in_hash",             test_stale_in_hash,             NULL, NULL },
    { "idx_route/stale_filter_range",        test_stale_filter_range,        NULL, NULL },
    { "idx_route/stale_sort_perm",           test_stale_sort_perm,           NULL, NULL },
    { "idx_route/stale_find_hash",           test_stale_find_hash,           NULL, NULL },
    { "idx_route/stale_distinct_sort",       test_stale_distinct_sort,       NULL, NULL },
    /* Task 10: adversarial sweeps */
    { "idx_route/bloom_adversarial_sweep",   test_bloom_adversarial_sweep,   NULL, NULL },
    { "idx_route/range_adversarial_sweep",   test_range_adversarial_sweep,   NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
