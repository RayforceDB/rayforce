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
    int64_t* vid = malloc(cap * sizeof *vid);
    int64_t* iid = malloc(cap * sizeof *iid);
    int64_t* sid = malloc(cap * sizeof *sid);
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
    { NULL, NULL, NULL, NULL },
};
