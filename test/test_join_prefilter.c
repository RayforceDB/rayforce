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

/* test/test_join_prefilter.c — #595 right-side key bitmap prefilter.
 *
 * The chained join probe skips a left row whose value, on some key column,
 * no right row holds.  Every test runs the same Rayfall join twice — once
 * with ray_join_no_prefilter set (the plain probe, the oracle) and once
 * without — and requires the two results to be identical cell for cell and
 * in order (a skip never reorders the chained probe's output).  The
 * counters say whether the filter was built and how many rows it skipped,
 * so a test that expects the filter cannot pass by silently not using it. */

#include "test.h"
#include "rayforce.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "lang/internal.h"
#include "lang/env.h"
#include "table/sym.h"
#include "table/domain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JP_TMP "/tmp/rayforce_test_join_prefilter"

static void jp_setup(void) {
    ray_runtime_create(0, NULL);
    (void)ray_test_rm_rf(ray_test_tmp(JP_TMP));
    (void)ray_test_mkdir_p(ray_test_tmp(JP_TMP));
}
static void jp_teardown(void) {
    ray_join_no_prefilter = false;
    ray_join_set_par_min(RAY_JOIN_PAR_MIN_DEFAULT);
    (void)ray_test_rm_rf(ray_test_tmp(JP_TMP));
    ray_runtime_destroy(__RUNTIME);
}

/* ── Fixtures ─────────────────────────────────────────────────────────────
 * Books are (venue, instrument) pairs: instrument i lives on venue i % NV.
 * Left row r carries book r % NI.  A null SYM is id 0, a null integer is
 * set through ray_vec_set_null. */
#define JP_NV 6
#define JP_NI 97

static int64_t jp_sym(const char* s) {
    return s[0] ? ray_sym_intern(s, strlen(s)) : 0;
}
static int64_t jp_inst(int64_t i) {
    char b[32]; snprintf(b, sizeof b, "INST%lld", (long long)i); return jp_sym(b);
}
static int64_t jp_venue(int64_t i) {
    char b[32]; snprintf(b, sizeof b, "V%lld", (long long)(i % JP_NV)); return jp_sym(b);
}

static ray_t* jp_sym_vec(uint8_t w, const int64_t* ids, int64_t n) {
    ray_t* v = ray_sym_vec_new(w, n);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = n;
    for (int64_t i = 0; i < n; i++)
        ray_write_sym(ray_data(v), i, (uint64_t)ids[i], RAY_SYM, v->attrs);
    return v;
}

static ray_t* jp_add(ray_t* tbl, const char* name, ray_t* col) {
    tbl = ray_table_add_col(tbl, ray_sym_intern(name, strlen(name)), col);
    ray_release(col);
    return tbl;
}

/* Table [venue instrument sid px] for the books in `book[0..n)` (-1 = the
 * all-null key row).  sid is I64 (or I32 with sid32) = book % 5. */
static ray_t* jp_table(const int64_t* book, int64_t n, uint8_t wv, uint8_t wi,
                       bool sid32) {
    size_t cap = (size_t)(n ? n : 1);
    int64_t* vid = calloc(cap, sizeof *vid);
    int64_t* iid = calloc(cap, sizeof *iid);
    int64_t* sid = calloc(cap, sizeof *sid);
    int64_t* px  = malloc(cap * sizeof *px);
    if (!vid || !iid || !sid || !px) {
        free(vid); free(iid); free(sid); free(px);
        return ray_error("oom", NULL);
    }
    for (int64_t r = 0; r < n; r++) {
        int64_t b = book[r];
        vid[r] = b < 0 ? 0 : jp_venue(b);
        iid[r] = b < 0 ? 0 : jp_inst(b);
        sid[r] = b < 0 ? 0 : b % 5;
        px[r]  = r;
    }
    ray_t* t = ray_table_new(4);
    t = jp_add(t, "venue", jp_sym_vec(wv, vid, n));
    t = jp_add(t, "instrument", jp_sym_vec(wi, iid, n));
    ray_t* s;
    if (sid32) {
        s = ray_vec_new(RAY_I32, n);
        s->len = n;
        for (int64_t r = 0; r < n; r++) ((int32_t*)ray_data(s))[r] = (int32_t)sid[r];
    } else {
        s = ray_vec_from_raw(RAY_I64, sid, n);
    }
    for (int64_t r = 0; r < n; r++)
        if (book[r] < 0) ray_vec_set_null(s, r, true);
    t = jp_add(t, "sid", s);
    t = jp_add(t, "px", ray_vec_from_raw(RAY_I64, px, n));
    free(vid); free(iid); free(sid); free(px);
    return t;
}

/* Left: n rows over all JP_NI books; every null_every-th row (0 = none) is
 * the all-null key row. */
static ray_t* jp_left(int64_t n, uint8_t wv, uint8_t wi, int64_t null_every) {
    int64_t* book = malloc((size_t)n * sizeof *book);
    if (!book) return ray_error("oom", NULL);
    for (int64_t r = 0; r < n; r++)
        book[r] = (null_every && r % null_every == 0) ? -1 : r % JP_NI;
    ray_t* t = jp_table(book, n, wv, wi, false);
    free(book);
    return t;
}

static void jp_bind(const char* name, ray_t* tbl) {
    (void)ray_env_set(ray_sym_intern(name, strlen(name)), tbl);
    ray_release(tbl);
}

/* ── Comparison ───────────────────────────────────────────────────────── */
static bool jp_cell_eq(ray_t* a, ray_t* b, int64_t i) {
    bool na = ray_vec_is_null(a, i), nb = ray_vec_is_null(b, i);
    if (na || nb) return na == nb;
    if (a->type == RAY_SYM) return sym_cell_runtime_id(a, i) == sym_cell_runtime_id(b, i);
    if (a->type == RAY_F64) return ((double*)ray_data(a))[i] == ((double*)ray_data(b))[i];
    return read_col_i64(ray_data(a), i, a->type, a->attrs) ==
           read_col_i64(ray_data(b), i, b->type, b->attrs);
}

static test_result_t jp_tables_equal(ray_t* a, ray_t* b) {
    TEST_ASSERT(a && !RAY_IS_ERR(a), "prefiltered join failed");
    TEST_ASSERT(b && !RAY_IS_ERR(b), "oracle join failed");
    TEST_ASSERT_EQ_I(ray_table_ncols(a), ray_table_ncols(b));
    TEST_ASSERT_EQ_I(ray_table_nrows(a), ray_table_nrows(b));
    int64_t nr = ray_table_nrows(a);
    for (int64_t c = 0; c < ray_table_ncols(a); c++) {
        TEST_ASSERT_EQ_I(ray_table_col_name(a, c), ray_table_col_name(b, c));
        ray_t* ca = ray_table_get_col_idx(a, c);
        ray_t* cb = ray_table_get_col_idx(b, c);
        TEST_ASSERT_EQ_I(ca->type, cb->type);
        for (int64_t i = 0; i < nr; i++) {
            if (!jp_cell_eq(ca, cb, i)) {
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "cell mismatch at row %lld col %lld", (long long)i, (long long)c);
                return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
            }
        }
    }
    PASS();
}

/* Run `src` with the prefilter off (oracle) and on.  expect: 1 = the filter
 * must be built and skip rows, 0 = it must not be built.  *rows_out gets the
 * result's row count. */
static test_result_t jp_diff(const char* src, int expect, int64_t* rows_out) {
    ray_join_no_prefilter = true;
    ray_t* oracle = ray_eval_str(src);
    ray_join_no_prefilter = false;

    uint64_t b0 = ray_join_prefilter_builds;
    uint64_t s0 = atomic_load(&ray_join_prefilter_skips);
    ray_t* got = ray_eval_str(src);
    bool built = ray_join_prefilter_builds > b0;
    bool skipped = atomic_load(&ray_join_prefilter_skips) > s0;

    test_result_t r = jp_tables_equal(got, oracle);
    if (r.status == TEST_PASS && expect == 1 && !(built && skipped)) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                 "prefilter expected to build and skip rows: %s", src);
        r = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }
    if (r.status == TEST_PASS && expect == 0 && built) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                 "prefilter built where it cannot help: %s", src);
        r = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }
    if (rows_out) *rows_out = (got && !RAY_IS_ERR(got)) ? ray_table_nrows(got) : -1;
    ray_release(got);
    ray_release(oracle);
    return r;
}

#define JP_DIFF(src, expect, rows_var)                                         \
    do {                                                                       \
        test_result_t _r = jp_diff((src), (expect), &(rows_var));              \
        if (_r.status != TEST_PASS) return _r;                                 \
    } while (0)

/* A small right side: the books named in `books[0..n)`. */
static void jp_bind_right(const char* name, const int64_t* books, int64_t n,
                          uint8_t wv, uint8_t wi, bool sid32) {
    jp_bind(name, jp_table(books, n, wv, wi, sid32));
}

/* ── Tests ────────────────────────────────────────────────────────────── */

#define JP_NL 40000

/* The reported shape: [venue instrument] over a large left side against a
 * few books, inner and left.  Left keys W16/W32, right keys W64/W16, so the
 * two sides read the same ids at different widths. */
static test_result_t test_jp_two_key_mixed_widths(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W16, RAY_SYM_W32, 0));
    static const int64_t books[] = { 3, 40, 41 };
    jp_bind_right("R", books, 3, RAY_SYM_W64, RAY_SYM_W16, false);

    int64_t rows = 0;
    JP_DIFF("(inner-join [venue instrument] L R)", 1, rows);
    /* each book holds JP_NL / JP_NI or one more rows */
    int64_t want = 0;
    for (int b = 0; b < 3; b++) want += JP_NL / JP_NI + (books[b] < JP_NL % JP_NI);
    TEST_ASSERT_EQ_I(rows, want);
    JP_DIFF("(left-join [venue instrument] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);

    /* the skip count is exact: every row of another book is rejected */
    uint64_t s0 = atomic_load(&ray_join_prefilter_skips);
    ray_t* r = ray_eval_str("(inner-join [venue instrument] L R)");
    TEST_ASSERT(r && !RAY_IS_ERR(r), "inner join");
    TEST_ASSERT_EQ_I((int64_t)(atomic_load(&ray_join_prefilter_skips) - s0), JP_NL - want);
    ray_release(r);
    PASS();
}

/* Three keys, the third an integer at different widths (I64 left, I32
 * right), and a right side whose books differ from a left book only in the
 * venue or only in sid — the filter narrows, the hash probe decides. */
static test_result_t test_jp_three_key(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W32, RAY_SYM_W32, 0));
    static const int64_t books[] = { 7, 12, 50, 51, 52 };
    jp_bind_right("R", books, 5, RAY_SYM_W16, RAY_SYM_W64, true);
    /* a pair no left row holds: instrument 7 on book 8's venue */
    ray_t* odd = ray_eval_str(
        "(table [venue instrument sid px] (list (at (at L 'venue) [8]) (at (at L 'instrument) [7]) (as 'I32 [2]) [0]))");
    TEST_ASSERT(odd && !RAY_IS_ERR(odd), "odd book");
    jp_bind("O", odd);

    int64_t rows = 0;
    JP_DIFF("(inner-join [venue instrument sid] L R)", 1, rows);
    TEST_ASSERT(rows > 0, "three-key inner join matches");
    JP_DIFF("(left-join [venue instrument sid] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(inner-join [venue instrument sid] L O)", 1, rows);
    TEST_ASSERT_EQ_I(rows, 0);
    JP_DIFF("(left-join [venue instrument sid] L O)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    /* two keys of the three, in another order */
    JP_DIFF("(inner-join [sid instrument] L R)", 1, rows);
    TEST_ASSERT(rows > 0, "reordered keys match");
    PASS();
}

/* Null keys: null equals null.  Left rows whose whole key is null must find
 * the right side's all-null row and only it; with no null on the right they
 * must find nothing. */
static test_result_t test_jp_null_keys(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W64, RAY_SYM_W64, 11));
    static const int64_t with_null[] = { 5, -1, 9 };
    static const int64_t no_null[]   = { 5, 9 };
    jp_bind_right("R", with_null, 3, RAY_SYM_W64, RAY_SYM_W64, false);
    jp_bind_right("S", no_null, 2, RAY_SYM_W64, RAY_SYM_W64, false);

    int64_t nulls = (JP_NL + 10) / 11;
    int64_t rows = 0, rows_s = 0;
    JP_DIFF("(inner-join [venue instrument sid] L R)", 1, rows);
    JP_DIFF("(inner-join [venue instrument sid] L S)", 1, rows_s);
    TEST_ASSERT_EQ_I(rows - rows_s, nulls);
    JP_DIFF("(left-join [venue instrument sid] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(left-join [venue instrument sid] L S)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    /* single nullable SYM key and single nullable integer key */
    JP_DIFF("(inner-join [venue] L R)", 1, rows);
    JP_DIFF("(inner-join [sid] L S)", 1, rows);
    JP_DIFF("(left-join [sid] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    PASS();
}

/* An empty right side: inner is empty, left keeps every row unmatched. */
static test_result_t test_jp_empty_right(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W16, RAY_SYM_W16, 0));
    jp_bind("R", jp_table(NULL, 0, RAY_SYM_W64, RAY_SYM_W64, false));
    int64_t rows = 0;
    uint64_t s0 = atomic_load(&ray_join_prefilter_skips);
    JP_DIFF("(inner-join [venue instrument] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, 0);
    TEST_ASSERT_EQ_I((int64_t)(atomic_load(&ray_join_prefilter_skips) - s0), JP_NL);
    JP_DIFF("(left-join [venue instrument] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    PASS();
}

/* A right side none of whose keys the left holds. */
static test_result_t test_jp_no_matches(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W32, RAY_SYM_W32, 0));
    ray_t* r = ray_eval_str(
        "(table [venue instrument sid px] (list (as 'SYM [\"V0\" \"VX\"]) (as 'SYM [\"NOPE1\" \"NOPE2\"]) [0 1] [0 1]))");
    TEST_ASSERT(r && !RAY_IS_ERR(r), "right table");
    jp_bind("R", r);
    int64_t rows = 0;
    JP_DIFF("(inner-join [venue instrument] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, 0);
    JP_DIFF("(left-join [venue instrument] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(inner-join [instrument] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, 0);
    PASS();
}

/* Every left row matches, or nearly: the sample shows the filter would
 * reject too little, so it is not built, and the answers are unchanged. */
static test_result_t test_jp_all_match(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W16, RAY_SYM_W16, 0));
    int64_t books[JP_NI];
    for (int64_t b = 0; b < JP_NI; b++) books[b] = b;
    jp_bind_right("R", books, JP_NI, RAY_SYM_W64, RAY_SYM_W64, false);
    int64_t rows = 0;
    JP_DIFF("(inner-join [venue instrument] L R)", 0, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(left-join [venue instrument] L R)", 0, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(inner-join [venue instrument sid] L R)", 0, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);

    /* All but two books: the filter would reject ~2% of the left rows, too
     * few to pay for a bit test on every row, so it is not built either. */
    jp_bind_right("N", books + 2, JP_NI - 2, RAY_SYM_W64, RAY_SYM_W64, false);
    JP_DIFF("(inner-join [venue instrument] L N)", 0, rows);
    TEST_ASSERT(rows > JP_NL * 9 / 10 && rows < JP_NL, "nearly every row matches");
    JP_DIFF("(left-join [venue instrument] L N)", 0, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    PASS();
}

/* FILE-domain SYM keys from a splayed table (narrow cell ids, positions in
 * the table's own domain) against runtime-domain right keys: the filter
 * must compare in runtime ids.  And against a right side loaded from the
 * same symfile (one domain): raw ids. */
static test_result_t test_jp_file_domain(void) {
    jp_bind("L0", jp_left(JP_NL, RAY_SYM_W64, RAY_SYM_W64, 13));
    static const int64_t books[] = { 2, 30, -1, 77 };
    jp_bind_right("R", books, 4, RAY_SYM_W64, RAY_SYM_W64, false);

    ray_t* e = ray_eval_str(ray_test_tmp(
        "(.db.splayed.set \"" JP_TMP "/l/\" L0 \"" JP_TMP "/.sym\")"));
    TEST_ASSERT(e && !RAY_IS_ERR(e), "save left");
    ray_release(e);
    e = ray_eval_str(ray_test_tmp(
        "(.db.splayed.set \"" JP_TMP "/r/\" R \"" JP_TMP "/.sym\")"));
    TEST_ASSERT(e && !RAY_IS_ERR(e), "save right");
    ray_release(e);
    ray_t* lf = ray_eval_str(ray_test_tmp(
        "(.db.splayed.get \"" JP_TMP "/l/\" \"" JP_TMP "/.sym\")"));
    TEST_ASSERT(lf && !RAY_IS_ERR(lf), "load left");
    ray_t* rf = ray_eval_str(ray_test_tmp(
        "(.db.splayed.get \"" JP_TMP "/r/\" \"" JP_TMP "/.sym\")"));
    TEST_ASSERT(rf && !RAY_IS_ERR(rf), "load right");

    ray_t* lv = ray_table_col(lf, ray_sym_intern("instrument", 10));
    TEST_ASSERT(lv && lv->type == RAY_SYM, "splayed SYM key");
    TEST_ASSERT(ray_sym_vec_domain(lv) != ray_sym_runtime_domain(),
                "splayed SYM key is in a file domain");
    TEST_ASSERT_EQ_I(lv->attrs & RAY_SYM_W_MASK, RAY_SYM_W8);   /* the W8 reader */
    ray_t* rv = ray_table_col(rf, ray_sym_intern("instrument", 10));
    TEST_ASSERT(rv && ray_sym_vec_domain(rv) == ray_sym_vec_domain(lv),
                "both splayed tables share one domain");
    /* The same FILE-domain ids widened to W64: an 8-byte column that still
     * needs translation, so it must not take the untranslated 8-byte probe
     * variant (join_pf_single64 requires !lut). */
    int64_t nl = ray_table_nrows(lf);
    ray_t* w64 = ray_sym_vec_new(RAY_SYM_W64, nl);
    TEST_ASSERT(w64 && !RAY_IS_ERR(w64), "W64 column");
    w64->len = nl;
    for (int64_t i = 0; i < nl; i++)
        ray_write_sym(ray_data(w64), i,
                      (uint64_t)ray_read_sym(ray_data(lv), i, RAY_SYM, lv->attrs),
                      RAY_SYM, w64->attrs);
    ray_sym_vec_adopt_domain(w64, lv);
    TEST_ASSERT(ray_sym_vec_domain(w64) == ray_sym_vec_domain(lv), "W64 keeps the file domain");
    ray_t* lw = jp_add(ray_table_new(1), "instrument", w64);
    jp_bind("LW", lw);
    jp_bind("LF", lf);
    jp_bind("RF", rf);

    int64_t rows_w = 0;
    JP_DIFF("(inner-join [instrument] LW R)", 1, rows_w);
    TEST_ASSERT(rows_w > 0, "W64 file-domain keys match runtime keys");
    JP_DIFF("(left-join [instrument] LW R)", 1, rows_w);
    TEST_ASSERT_EQ_I(rows_w, JP_NL);

    int64_t rows = 0, rows_f = 0;
    JP_DIFF("(inner-join [venue instrument] LF R)", 1, rows);
    JP_DIFF("(inner-join [venue instrument] LF RF)", 1, rows_f);
    TEST_ASSERT_EQ_I(rows, rows_f);
    TEST_ASSERT(rows > 0, "file-domain keys match runtime keys");
    JP_DIFF("(left-join [venue instrument sid] LF R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(left-join [venue instrument sid] LF RF)", 1, rows);
    TEST_ASSERT_EQ_I(rows, JP_NL);
    JP_DIFF("(inner-join [venue instrument sid] L0 RF)", 1, rows);
    JP_DIFF("(inner-join [venue instrument sid] LF R)", 1, rows_f);
    TEST_ASSERT_EQ_I(rows, rows_f);
    PASS();
}

/* A single integer key far from zero: the bitmap is offset by the smallest
 * right value, and left values below it are rejected as well as above. */
static test_result_t test_jp_int_offset(void) {
    ray_t* t = ray_eval_str(
        "(set L (table [k v] (list (+ 999999900 (% (til 40000) 3000)) (til 40000))))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "left");
    ray_release(t);
    t = ray_eval_str("(set R (table [k w] (list (as 'I32 [1000000005 1000002999]) [1 2])))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "right");
    ray_release(t);
    int64_t rows = 0;
    JP_DIFF("(inner-join [k] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, 14);   /* 999999900 + 105: 13 full cycles + 1 */
    JP_DIFF("(left-join [k] L R)", 1, rows);
    TEST_ASSERT_EQ_I(rows, 40000);
    PASS();
}

/* One integer key column of `type` (I16, DATE, TIME, U8, BOOL) holding
 * vals[0..n), with a null at every row where nulls[r] is set. */
static ray_t* jp_int_vec(int8_t type, const int64_t* vals, const bool* nulls, int64_t n) {
    ray_t* v = ray_vec_new(type, n ? n : 1);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = n;
    for (int64_t r = 0; r < n; r++) {
        switch (type) {
        case RAY_I16: ((int16_t*)ray_data(v))[r] = (int16_t)vals[r]; break;
        case RAY_DATE: case RAY_TIME: case RAY_I32:
                      ((int32_t*)ray_data(v))[r] = (int32_t)vals[r]; break;
        default:      ((uint8_t*)ray_data(v))[r] = (uint8_t)vals[r]; break;
        }
    }
    for (int64_t r = 0; r < n; r++)
        if (nulls && nulls[r]) ray_vec_set_null(v, r, true);
    return v;
}

/* Every cell width the filter reads, besides SYM (two_key_mixed_widths,
 * file_domain) and I64 (int_offset).  Signed classes carry negative values
 * and a null on both sides: a null must find the right side's null, a
 * negative must find its match, so reading the cells at the wrong width or
 * signedness changes the answer.  U8 and BOOL cannot hold a null. */
static test_result_t test_jp_width_classes(void) {
    static const int8_t types[] = { RAY_I16, RAY_DATE, RAY_TIME, RAY_U8, RAY_BOOL };
    int64_t* lv = malloc(JP_NL * sizeof *lv);
    bool*    ln = malloc(JP_NL * sizeof *ln);
    TEST_ASSERT(lv && ln, "fixture allocation");
    for (size_t t = 0; t < sizeof types / sizeof *types; t++) {
        int8_t ty = types[t];
        bool sgn = ty != RAY_U8 && ty != RAY_BOOL;
        for (int64_t r = 0; r < JP_NL; r++) {
            lv[r] = ty == RAY_BOOL ? r % 2 : sgn ? r % 97 - 10 : r % 97;
            ln[r] = sgn && r % 11 == 0;
        }
        static const int64_t rv_s[] = { 3, 50, -5, 0 };
        static const bool    rn_s[] = { false, false, false, true };
        static const int64_t rv_u[] = { 3, 50 };
        static const int64_t rv_b[] = { 1 };
        const int64_t* rv = ty == RAY_BOOL ? rv_b : sgn ? rv_s : rv_u;
        int64_t rn = ty == RAY_BOOL ? 1 : sgn ? 4 : 2;
        int64_t* px = malloc(JP_NL * sizeof *px);
        TEST_ASSERT(px, "px");
        for (int64_t r = 0; r < JP_NL; r++) px[r] = r;
        ray_t* lt = ray_table_new(2);
        lt = jp_add(lt, "k", jp_int_vec(ty, lv, ln, JP_NL));
        lt = jp_add(lt, "px", ray_vec_from_raw(RAY_I64, px, JP_NL));
        ray_t* rt = ray_table_new(2);
        rt = jp_add(rt, "k", jp_int_vec(ty, rv, sgn ? rn_s : NULL, rn));
        rt = jp_add(rt, "w", ray_vec_from_raw(RAY_I64, px, rn));
        free(px);
        TEST_ASSERT_EQ_I(ray_table_get_col_idx(lt, 0)->type, ty);
        jp_bind("L", lt);
        jp_bind("R", rt);

        int64_t rows = 0, want = 0;
        for (int64_t r = 0; r < JP_NL; r++) {
            if (ln[r]) { want += sgn; continue; }       /* null finds the null */
            for (int64_t j = 0; j < rn; j++)
                if (!(sgn && rn_s[j]) && rv[j] == lv[r]) want++;
        }
        JP_DIFF("(inner-join [k] L R)", 1, rows);
        TEST_ASSERT_EQ_I(rows, want);
        JP_DIFF("(left-join [k] L R)", 1, rows);
        TEST_ASSERT_EQ_I(rows, JP_NL);
    }
    free(lv); free(ln);
    PASS();
}

/* The knob alone turns the filter off. */
static test_result_t test_jp_knob(void) {
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W16, RAY_SYM_W16, 0));
    static const int64_t books[] = { 1 };
    jp_bind_right("R", books, 1, RAY_SYM_W16, RAY_SYM_W16, false);
    ray_join_no_prefilter = true;
    uint64_t b0 = ray_join_prefilter_builds;
    ray_t* r = ray_eval_str("(inner-join [venue instrument] L R)");
    ray_join_no_prefilter = false;
    TEST_ASSERT(r && !RAY_IS_ERR(r), "join");
    TEST_ASSERT_EQ_I((int64_t)(ray_join_prefilter_builds - b0), 0);
    ray_release(r);
    b0 = ray_join_prefilter_builds;
    r = ray_eval_str("(inner-join [venue instrument] L R)");
    TEST_ASSERT(r && !RAY_IS_ERR(r), "join");
    TEST_ASSERT_EQ_I((int64_t)(ray_join_prefilter_builds - b0), 1);
    ray_release(r);
    PASS();
}

/* #599: run `src` with the probe floor at `floor`; *probes gets the number of
 * chained-probe passes that went to the pool: 2 (count and fill) for a
 * dispatched join with matches, 0 for a serial one. */
static ray_t* jp_at_floor(const char* src, int64_t floor, uint64_t* probes) {
    ray_join_set_par_min(floor);
    uint64_t p0 = atomic_load(&ray_join_par_probes);
    ray_t* r = ray_eval_str(src);
    *probes = atomic_load(&ray_join_par_probes) - p0;
    ray_join_set_par_min(RAY_JOIN_PAR_MIN_DEFAULT);
    return r;
}

/* #599: the probe floor decides where the chained probe runs and nothing
 * else.  At the default a probe of more than one morsel is dispatched; with
 * the floor above the probe side it runs on this thread; the floor is
 * inclusive; a one-morsel probe never splits, even at floor 0.  Every answer
 * is the default's, cell for cell and in order. */
static test_result_t test_jp_par_min(void) {
    if (!ray_pool_get()) SKIP("no pool");
    TEST_ASSERT_EQ_I(ray_join_par_min(), RAY_JOIN_PAR_MIN_DEFAULT);
    jp_bind("L", jp_left(JP_NL, RAY_SYM_W16, RAY_SYM_W32, 0));
    jp_bind("S", jp_left(8192, RAY_SYM_W16, RAY_SYM_W32, 0));
    static const int64_t books[] = { 3, 40, 41 };
    jp_bind_right("R", books, 3, RAY_SYM_W64, RAY_SYM_W16, false);

    static const char* const srcs[] = {
        "(inner-join [venue instrument] L R)",
        "(left-join [venue instrument] L R)",
        "(full-join [venue instrument] L R)",
    };
    /* floor -> dispatched passes.  JP_NL is 40000 rows, five morsels; every
     * join here has matches, so a dispatched one sends both count and fill. */
    static const struct { int64_t floor; uint64_t want; } cases[] = {
        { RAY_JOIN_PAR_MIN_DEFAULT, 2 }, { 0, 2 }, { JP_NL, 2 },
        { JP_NL + 1, 0 }, { 65536, 0 }, { INT64_MAX, 0 },
    };
    for (size_t s = 0; s < sizeof srcs / sizeof srcs[0]; s++) {
        uint64_t probes = 0;
        ray_join_no_prefilter = false;
        ray_t* want = jp_at_floor(srcs[s], RAY_JOIN_PAR_MIN_DEFAULT, &probes);
        TEST_ASSERT(want && !RAY_IS_ERR(want), "default join");
        TEST_ASSERT_EQ_I((int64_t)probes, 2);
        for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            ray_t* got = jp_at_floor(srcs[s], cases[c].floor, &probes);
            test_result_t r = jp_tables_equal(got, want);
            ray_release(got);
            if (r.status != TEST_PASS) { ray_release(want); return r; }
            if (probes != cases[c].want) {
                ray_release(want);
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "floor %lld: %llu dispatched probes, want %llu: %s",
                         (long long)cases[c].floor, (unsigned long long)probes,
                         (unsigned long long)cases[c].want, srcs[s]);
                return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
            }
        }
        ray_release(want);
    }

    /* One morsel: serial at every floor. */
    uint64_t probes = 0;
    ray_t* r = jp_at_floor("(inner-join [venue instrument] S R)", 0, &probes);
    TEST_ASSERT(r && !RAY_IS_ERR(r), "one-morsel join");
    TEST_ASSERT_EQ_I((int64_t)probes, 0);
    ray_release(r);
    PASS();
}

/* ── SYM key domains ──────────────────────────────────────────────────────
 * A FILE-domain key's ids are positions in its symfile.  The join compares
 * a same-domain pair by raw id and translates only the rows of a
 * cross-domain pair by string lookup (join_align_sym_keys), so no file
 * vocabulary is interned into the runtime table: ray_sym_count is the
 * observable, over vocabularies appended straight to their symfiles (never
 * through the runtime table).  Both the radix path (right side above
 * RAY_PARALLEL_THRESHOLD rows) and the chained one, both translation
 * directions, two symfiles, absent strings, null keys, and the full-join
 * key fill, which keeps the left column's domain. */

/* Table [k <val>] over `dom`: row r holds position pos[r] (W16). */
static ray_t* jsd_table(ray_sym_domain_t* dom, const int64_t* pos, int64_t n,
                        const char* val_name) {
    ray_t* k = jp_sym_vec(RAY_SYM_W16, pos, n);
    if (!k || RAY_IS_ERR(k)) return k;
    ray_sym_domain_retain(dom);
    k->sym_domain = dom;
    int64_t* v = malloc((size_t)n * sizeof *v);
    if (!v) { ray_release(k); return ray_error("oom", NULL); }
    for (int64_t r = 0; r < n; r++) v[r] = r;
    ray_t* t = ray_table_new(2);
    t = jp_add(t, "k", k);
    t = jp_add(t, val_name, ray_vec_from_raw(RAY_I64, v, n));
    free(v);
    return t;
}

static ray_t* jsd_eval_rows(const char* src, int64_t* rows) {
    ray_t* t = ray_eval_str(src);
    *rows = (t && !RAY_IS_ERR(t)) ? ray_table_nrows(t) : -1;
    return t;
}

#define JSD_NV   1000     /* vocabulary of each symfile */
#define JSD_BIG  70000    /* rows of the big table: above RAY_PARALLEL_THRESHOLD */
#define JSD_NV3  2500     /* vocabulary of the third symfile */
#define JSD_BIG3 60000    /* rows of the table over it: fewer than BIG */

static test_result_t test_jp_sym_domains(void) {
    _Static_assert(JSD_BIG > RAY_PARALLEL_THRESHOLD, "the big side must take the radix path");
    /* Two symfiles: d1 holds jsd0..jsd999, d2 holds jsd500..jsd1499; the
     * strings never pass through the runtime table. */
    ray_sym_domain_t* d1 = ray_sym_domain_open_or_create(ray_test_tmp(JP_TMP "/d1.sym"));
    ray_sym_domain_t* d2 = ray_sym_domain_open_or_create(ray_test_tmp(JP_TMP "/d2.sym"));
    TEST_ASSERT(d1 && d2 && d1 != d2, "two symfile domains");
    static int64_t p1[JSD_NV], p2[JSD_NV];
    char buf[32];
    for (int64_t i = 0; i < JSD_NV; i++) {
        int n = snprintf(buf, sizeof buf, "jsd%lld", (long long)i);
        p1[i] = ray_sym_domain_intern(d1, buf, (size_t)n);
        n = snprintf(buf, sizeof buf, "jsd%lld", (long long)(i + JSD_NV / 2));
        p2[i] = ray_sym_domain_intern(d2, buf, (size_t)n);
        TEST_ASSERT(p1[i] > 0 && p2[i] > 0, "appended to the symfile");
    }
    TEST_ASSERT_EQ_I(ray_sym_find("jsd0", 4), -1);   /* foreign to the runtime table */

    /* BIG: 70000 rows over d1, key r % 1000 (70 rows per key).
     * SML: 10 rows over d1, key 7j; row 9 is the null key.
     * TWO: 20 rows over d2, jsd990..jsd1009: half of them are in d1.
     * RT:  runtime literals jsd5, jsd7, jsd_none and the null.
     * RT2: jsd5 and the null (every string of it is in d1). */
    static int64_t pos[JSD_BIG];
    for (int64_t r = 0; r < JSD_BIG; r++) pos[r] = p1[r % JSD_NV];
    jp_bind("BIG", jsd_table(d1, pos, JSD_BIG, "v"));
    for (int64_t j = 0; j < 10; j++) pos[j] = j == 9 ? 0 : p1[7 * j];
    jp_bind("SML", jsd_table(d1, pos, 10, "w"));
    for (int64_t j = 0; j < 20; j++) pos[j] = p2[JSD_NV / 2 - 10 + j];
    jp_bind("TWO", jsd_table(d2, pos, 20, "u"));
    ray_sym_domain_release(d1);
    ray_sym_domain_release(d2);
    ray_t* e = ray_eval_str("(set RT (table [k x] (list (as 'SYM [\"jsd5\" \"jsd7\" \"jsd_none\" \"\"]) [1 2 3 4])))");
    TEST_ASSERT(e && !RAY_IS_ERR(e), "RT");
    ray_release(e);
    e = ray_eval_str("(set RT2 (table [k x] (list (as 'SYM [\"jsd5\" \"\"]) [1 4])))");
    TEST_ASSERT(e && !RAY_IS_ERR(e), "RT2");
    ray_release(e);
    ray_t* big_k = ray_table_col(ray_env_get(ray_sym_intern("BIG", 3)), ray_sym_intern("k", 1));
    TEST_ASSERT(big_k && ray_sym_vec_domain(big_k) == d1, "BIG keys resolve over d1");

    uint32_t c0 = ray_sym_count();
    int64_t rows = 0;
    /* same domain, radix (right = BIG) and chained (right = SML) */
    ray_t* t = jsd_eval_rows("(inner-join [k] SML BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 9 * 70);
    ray_release(t);
    t = jsd_eval_rows("(inner-join [k] BIG SML)", &rows);
    TEST_ASSERT_EQ_I(rows, 9 * 70);
    ray_release(t);
    t = jsd_eval_rows("(left-join [k] SML BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 9 * 70 + 1);
    ray_release(t);
    /* two symfiles: jsd990..jsd999 are in d1, jsd1000..jsd1009 are not.
     * TWO (20 rows) is the side translated into d1: its 20 distinct ids are
     * looked up by string once; the pair's cache answers the repeat. */
    uint64_t finds0 = ray_join_test_sym_finds();
    t = jsd_eval_rows("(inner-join [k] TWO BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70);
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 20);
    t = jsd_eval_rows("(left-join [k] TWO BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70 + 10);
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 20);
    t = jsd_eval_rows("(inner-join [k] BIG TWO)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70);
    ray_release(t);
    t = jsd_eval_rows("(anti-join [k] TWO SML)", &rows);
    TEST_ASSERT_EQ_I(rows, 20);
    ray_release(t);
    /* a symfile against runtime literals: RT into d1 (the small side), SML
     * into the runtime table (SML is the small side against RT: its other
     * strings are looked up, not interned).  jsd_none is in no file. */
    t = jsd_eval_rows("(inner-join [k] RT BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 2 * 70);
    ray_release(t);
    t = jsd_eval_rows("(left-join [k] BIG RT)", &rows);
    TEST_ASSERT_EQ_I(rows, JSD_BIG);
    ray_release(t);
    t = jsd_eval_rows("(inner-join [k] SML RT)", &rows);
    TEST_ASSERT_EQ_I(rows, 2);                      /* jsd7, and null == null */
    ray_t* xk = ray_table_col(t, ray_sym_intern("k", 1));
    TEST_ASSERT(xk && ray_sym_vec_domain(xk) == d1, "result keys are the left cells, in d1");
    ray_release(t);
    t = jsd_eval_rows("(anti-join [k] RT SML)", &rows);
    TEST_ASSERT_EQ_I(rows, 2);                      /* jsd5, jsd_none */
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_sym_count(), c0);          /* nothing interned by any join */
    TEST_ASSERT_EQ_I(ray_sym_find("jsd0", 4), -1);

    /* full join, FILE left: the right-only jsd5 is stored by its d1
     * position; the key column keeps d1. */
    t = jsd_eval_rows("(full-join [k] SML RT2)", &rows);
    TEST_ASSERT_EQ_I(rows, 11);
    xk = ray_table_col(t, ray_sym_intern("k", 1));
    TEST_ASSERT(xk && ray_sym_vec_domain(xk) == d1, "full-join keys keep the left domain");
    TEST_ASSERT_EQ_I(ray_read_sym(ray_data(xk), 10, RAY_SYM, xk->attrs), p1[5]);
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_sym_count(), c0);
    /* full join, runtime left: the 8 right-only strings are interned, one
     * each, and nothing else of the 1000-entry vocabulary. */
    t = jsd_eval_rows("(full-join [k] RT SML)", &rows);
    TEST_ASSERT_EQ_I(rows, 4 + 8);
    xk = ray_table_col(t, ray_sym_intern("k", 1));
    TEST_ASSERT(xk && ray_sym_vec_domain(xk) == ray_sym_runtime_domain(), "runtime keys stay runtime");
    for (int64_t r = 4; r < 12; r++) {
        ray_t* s = ray_sym_str(ray_read_sym(ray_data(xk), r, RAY_SYM, xk->attrs));
        TEST_ASSERT(s && ray_str_len(s) >= 4 && memcmp(ray_str_ptr(s), "jsd", 3) == 0,
                    "right-only key resolves to its string");
        TEST_ASSERT(!(ray_str_len(s) == 4 && ray_str_ptr(s)[3] == '7'), "jsd7 matched, not right-only");
    }
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_sym_count(), c0 + 8);
    TEST_ASSERT_EQ_I(ray_sym_find("jsd1", 4), -1);

    /* Releasing a domain drops what was translated against it.  TWO's
     * table was the last holder of d2; a fresh symfile takes its place
     * under TWO, holding jsd0..jsd2499 at REVERSED positions (what a stale
     * cache for d2's address, were it reused, would mistranslate: TWO's
     * ids then name jsd1009..jsd990), and the joins over the new pair are
     * right.  BIG3 over it has fewer rows than BIG, so BIG3 is the side
     * translated: its 1500 distinct ids carry the pair's cache past its
     * first 1024 slots. */
    jp_bind("TWO", ray_table_new(0));
    ray_sym_domain_t* d3 = ray_sym_domain_open_or_create(ray_test_tmp(JP_TMP "/d3.sym"));
    TEST_ASSERT(d3, "a third symfile domain");
    static int64_t p3[JSD_NV3];
    for (int64_t i = JSD_NV3 - 1; i >= 0; i--) {
        int n = snprintf(buf, sizeof buf, "jsd%lld", (long long)i);
        p3[i] = ray_sym_domain_intern(d3, buf, (size_t)n);
        TEST_ASSERT(p3[i] > 0, "appended to the symfile");
    }
    for (int64_t j = 0; j < 20; j++) pos[j] = p3[JSD_NV - 10 + j];           /* jsd990..jsd1009 */
    jp_bind("TWO", jsd_table(d3, pos, 20, "u"));
    for (int64_t r = 0; r < JSD_BIG3; r++) pos[r] = p3[JSD_NV - 10 + r % 1500];  /* jsd990..jsd2489 */
    jp_bind("BIG3", jsd_table(d3, pos, JSD_BIG3, "y"));
    ray_sym_domain_release(d3);
    uint32_t c1 = ray_sym_count();                  /* the binds named BIG3 and y */
    finds0 = ray_join_test_sym_finds();
    t = jsd_eval_rows("(inner-join [k] TWO BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70);
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 20);
    /* the matched TWO rows are jsd990..jsd999: u = 0..9, 70 times each */
    t = ray_eval_str("(sum (at (inner-join [k] TWO BIG) 'u))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "sum");
    TEST_ASSERT_EQ_I(t->i64, 70 * 45);
    ray_release(t);
    t = jsd_eval_rows("(anti-join [k] TWO BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 10);
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 20);
    /* Under a byte bound the caches together may not pass, the pair's
     * table keeps what it holds (20 ids, 1024 slots) and takes 492 more
     * (its load bound); the other 988 ids are memoized for the call only.
     * (TWO's 20 ids are among BIG3's 1500 and already in the cache.) */
    size_t bytes0 = ray_sym_domain_test_xlat_bytes();
    ray_sym_domain_test_xlat_bound(bytes0 + 8 * 1024);     /* no room to double */
    t = jsd_eval_rows("(inner-join [k] BIG BIG3)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70 * (JSD_BIG3 / 1500));
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 1500);
    TEST_ASSERT_EQ_I(ray_sym_domain_test_xlat_bytes(), bytes0);
    t = jsd_eval_rows("(inner-join [k] BIG3 BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70 * (JSD_BIG3 / 1500));
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 1500 + 988);
    TEST_ASSERT_EQ_I(ray_sym_domain_test_xlat_bytes(), bytes0);
    /* room again: the next call keeps the 988 (1024 -> 4096 slots), the
     * one after is all hits */
    ray_sym_domain_test_xlat_bound(0);
    t = jsd_eval_rows("(inner-join [k] BIG3 BIG)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70 * (JSD_BIG3 / 1500));
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 1500 + 2 * 988);
    TEST_ASSERT_EQ_I(ray_sym_domain_test_xlat_bytes(), bytes0 + 3 * 8 * 1024);
    t = jsd_eval_rows("(inner-join [k] BIG BIG3)", &rows);
    TEST_ASSERT_EQ_I(rows, 10 * 70 * (JSD_BIG3 / 1500));
    ray_release(t);
    TEST_ASSERT_EQ_I(ray_join_test_sym_finds() - finds0, 1500 + 2 * 988);
    TEST_ASSERT_EQ_I(ray_sym_count(), c1);
    /* the null stays the null, a negative id (no cell holds one) names
     * nothing, neither is looked up */
    int64_t odd[3] = { -1, 0, p1[5] };
    TEST_ASSERT_EQ_I(ray_sym_domain_xlat(d1, ray_sym_runtime_domain(), odd, 3, odd), 1);  /* jsd5 only */
    TEST_ASSERT_EQ_I(odd[0], -1);
    TEST_ASSERT_EQ_I(odd[1], 0);
    TEST_ASSERT_EQ_I(odd[2], ray_sym_find("jsd5", 4));
    int64_t odd2[3] = { -1, 0, p1[5] };
    TEST_ASSERT_EQ_I(ray_sym_domain_xlat(d1, ray_sym_runtime_domain(), odd2, 3, odd2), 0);
    TEST_ASSERT_EQ_I(odd2[0], -1);
    TEST_ASSERT_EQ_I(odd2[2], odd[2]);
    PASS();
}

/* A FILE domain a holder keeps across a runtime restart (an embedding host
 * that re-creates the runtime): what was translated into the old runtime
 * table dies with it -- the new table assigns ids afresh. */
static test_result_t test_jp_sym_domains_restart(void) {
    ray_sym_domain_t* d = ray_sym_domain_open_or_create(ray_test_tmp(JP_TMP "/dr.sym"));
    TEST_ASSERT(d, "a symfile domain");
    static int64_t p[10];
    char buf[32];
    for (int64_t i = 0; i < 10; i++) {
        int n = snprintf(buf, sizeof buf, "jsr%lld", (long long)i);
        p[i] = ray_sym_domain_intern(d, buf, (size_t)n);
        TEST_ASSERT(p[i] > 0, "appended to the symfile");
    }
    /* F (10 rows over d) is the side translated into the runtime table */
    jp_bind("F", jsd_table(d, p, 10, "v"));
    ray_t* e = ray_eval_str("(set RT (table [k x] (list (as 'SYM [\"jsr3\" \"jsr5\" \"jsr_none\"]) [1 2 3])))");
    TEST_ASSERT(e && !RAY_IS_ERR(e), "RT");
    ray_release(e);
    int64_t rows = 0;
    ray_t* t = jsd_eval_rows("(inner-join [k] F RT)", &rows);
    TEST_ASSERT_EQ_I(rows, 2);
    ray_release(t);
    t = ray_eval_str("(sum (at (inner-join [k] F RT) 'x))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "sum");
    TEST_ASSERT_EQ_I(t->i64, 1 + 2);
    ray_release(t);

    /* the runtime restarts under d (this holder keeps it); the same three
     * strings are interned in the other order, so an id the old table gave
     * jsr3 now names another string */
    ray_runtime_destroy(__RUNTIME);
    ray_runtime_create(0, NULL);
    jp_bind("F", jsd_table(d, p, 10, "v"));
    e = ray_eval_str("(set RT (table [k x] (list (as 'SYM [\"jsr_none\" \"jsr5\" \"jsr3\"]) [3 2 1])))");
    TEST_ASSERT(e && !RAY_IS_ERR(e), "RT");
    ray_release(e);
    t = jsd_eval_rows("(inner-join [k] F RT)", &rows);
    TEST_ASSERT_EQ_I(rows, 2);
    ray_release(t);
    t = ray_eval_str("(sum (at (inner-join [k] F RT) 'x))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "sum");
    TEST_ASSERT_EQ_I(t->i64, 1 + 2);
    ray_release(t);
    ray_sym_domain_release(d);
    PASS();
}

const test_entry_t join_prefilter_entries[] = {
    { "join_prefilter/two_key_mixed_widths", test_jp_two_key_mixed_widths, jp_setup, jp_teardown },
    { "join_prefilter/three_key",            test_jp_three_key,            jp_setup, jp_teardown },
    { "join_prefilter/null_keys",            test_jp_null_keys,            jp_setup, jp_teardown },
    { "join_prefilter/empty_right",          test_jp_empty_right,          jp_setup, jp_teardown },
    { "join_prefilter/no_matches",           test_jp_no_matches,           jp_setup, jp_teardown },
    { "join_prefilter/all_match",            test_jp_all_match,            jp_setup, jp_teardown },
    { "join_prefilter/file_domain",          test_jp_file_domain,          jp_setup, jp_teardown },
    { "join_prefilter/int_offset",           test_jp_int_offset,           jp_setup, jp_teardown },
    { "join_prefilter/width_classes",        test_jp_width_classes,        jp_setup, jp_teardown },
    { "join_prefilter/knob",                 test_jp_knob,                 jp_setup, jp_teardown },
    { "join_prefilter/par_min",              test_jp_par_min,              jp_setup, jp_teardown },
    { "join_prefilter/sym_domains",          test_jp_sym_domains,          jp_setup, jp_teardown },
    { "join_prefilter/sym_domains_restart",  test_jp_sym_domains_restart,  jp_setup, jp_teardown },
    { NULL, NULL, NULL, NULL },
};
