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

/* Ragged parted tables (#724): a partition lacking a tail column reads as
 * that column's nulls, so every column has the table's row count.  Answers
 * on generation 0, generation 2 and mixed ragged tables equal those of the
 * same table written with explicit nulls in the missing rows. */

#define _POSIX_C_SOURCE 200809L
#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "core/runtime.h"
#include "lang/eval.h"
#include "table/sym.h"
#include "table/table.h"
#include "store/enccol.h"
#include "store/part.h"
#include "store/serde.h"
#include <stdio.h>
#include <string.h>

#define PR_TMP "/tmp/rayforce_parted_ragged"

static void pr_setup(void)    { ray_runtime_create(0, NULL); (void)ray_test_rm_rf(PR_TMP); (void)ray_test_mkdir_p(PR_TMP); }
static void pr_teardown(void) { (void)ray_test_rm_rf(PR_TMP); ray_runtime_destroy(__RUNTIME); }

static void drop_out(ray_t* o) { if (!o) return; if (RAY_IS_ERR(o)) ray_error_free(o); else ray_release(o); }

static bool ev_ok(const char* src) {
    ray_t* r = ray_eval_str(src);
    bool ok = r && !RAY_IS_ERR(r);
    drop_out(r);
    return ok;
}

/* Same serialized value and same printed form. */
static bool same_value(ray_t* x, ray_t* y) {
    if (!x || !y || RAY_IS_ERR(x) || RAY_IS_ERR(y)) return false;
    ray_t* sx = ray_ser(x); ray_t* sy = ray_ser(y);
    bool same = sx && sy && !RAY_IS_ERR(sx) && !RAY_IS_ERR(sy) && sx->len == sy->len &&
                memcmp(ray_data(sx), ray_data(sy), (size_t)sx->len) == 0;
    if (sx && !RAY_IS_ERR(sx)) ray_release(sx);
    if (sy && !RAY_IS_ERR(sy)) ray_release(sy);
    if (!same) return false;
    ray_t* fx = ray_fmt(x, 0); ray_t* fy = ray_fmt(y, 0);
    same = fx && fy && ray_str_len(fx) == ray_str_len(fy) &&
           memcmp(ray_str_ptr(fx), ray_str_ptr(fy), ray_str_len(fx)) == 0;
    if (fx) ray_release(fx);
    if (fy) ray_release(fy);
    return same;
}

/* Three partitions of t under `root`; bit p of `gen2` writes partition p as
 * generation 2.  `ragged` writes the last partition with column a alone;
 * otherwise it carries the other columns' nulls explicitly (the reference). */
static bool write_db(const char* root, unsigned gen2, bool ragged) {
    static const char* const parts[3] = {
        "(table [a b s f str d flag] (list [1 2 3] [10 20 30] ['x 'y 'x] [1.5 2.5 3.5] [\"p\" \"q\" \"r\"] "
        "[2024.01.01 2024.01.02 2024.01.03] [true false true]))",
        "(table [a b s f str d flag] (list [4 5] [40 50] ['y 'z] [4.5 5.5] [\"s\" \"t\"] "
        "[2024.02.01 2024.02.02] [false true]))",
        "(table [a b s f str d flag] (list [6 7 8 9] (take [0Nl] 4) (take ['] 4) (take [0Nf] 4) "
        "(take [\"\"] 4) (take [0Nd] 4) (take [false] 4)))",
    };
    for (int p = 0; p < 3; p++) {
        char q[1024];
        const char* tbl = (ragged && p == 2) ? "(table [a] (list [6 7 8 9]))" : parts[p];
        snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.0%d/t/\" %s {compress: %s})",
                 root, p + 1, tbl, ((gen2 >> p) & 1) ? "true" : "false");
        if (!ev_ok(q)) return false;
    }
    return true;
}

static ray_t* load_db(const char* root) {
    ray_t* t = ray_read_parted(root, "t");
    return (t && !RAY_IS_ERR(t)) ? t : NULL;
}

static void bind_t(ray_t* t) { ray_env_set(ray_sym_intern("t", 1), t); }

static const char* const queries[] = {
    "(select {s: (sum b) from: t})",
    "(select {c: (count a) by: b from: t where: (> a 0)})",
    "(select {b: b from: t where: (>= date 2024.01.02)})",
    "(select {b: b s: s from: t where: (== date 2024.01.03)})",
    "(select {s: (sum b) from: t where: (== date 2024.01.03)})",
    "(select {date: date a: a b: b s: s f: f str: str d: d flag: flag from: t})",
    "(select {from: t where: (> a 4)})",
    "(select {s: (sum f) m: (max d) n: (count s) x: (min b) by: date from: t})",
    "(select {c: (count a) by: s from: t})",
    "(select {c: (count a) by: str from: t})",
    "(select {c: (count a) by: flag from: t})",
    "(select {x: (+ a b) y: (* f 2) from: t})",
    "(xasc (select {a: a b: b from: t}) 'b)",
    "(xdesc (select {a: a d: d from: t}) 'd)",
    "(select {s: (sum a) from: t where: (> b 15)})",
    "(select {a: a from: t where: (== s 'x)})",
    "(select {c: (count (distinct b)) by: date from: t})",
    "(select {r: (sums b) from: t})",
    "(at t 'b)",
    "(at t 's)",
    "(at t 'str)",
    "(at t 'flag)",
    "(sum (at t 'b))",
    "(count (at t 'f))",
    NULL,
};

/* Every query answers on the ragged table exactly as on the reference. */
static test_result_t ragged_matches(unsigned gen2) {
    char rr[128], rf[128];
    snprintf(rr, sizeof rr, PR_TMP "/rag_%u", gen2);
    snprintf(rf, sizeof rf, PR_TMP "/ref_%u", gen2);
    TEST_ASSERT_TRUE(write_db(rr, gen2, true));
    TEST_ASSERT_TRUE(write_db(rf, gen2, false));
    ray_t* tr = load_db(rr);
    ray_t* tf = load_db(rf);
    TEST_ASSERT_TRUE(tr && tf);
    for (int i = 0; queries[i]; i++) {
        bind_t(tf);
        ray_t* want = ray_eval_str(queries[i]);
        bind_t(tr);
        ray_t* got = ray_eval_str(queries[i]);
        bool ok = want && !RAY_IS_ERR(want);
        bool same = same_value(want, got);
        drop_out(want); drop_out(got);
        TEST_ASSERT_FMT(ok, "gen2 %u: %s: reference answers", gen2, queries[i]);
        TEST_ASSERT_FMT(same, "gen2 %u: %s: ragged equals reference", gen2, queries[i]);
    }
    bind_t(NULL);
    ray_release(tr); ray_release(tf);
    PASS();
}

static test_result_t test_ragged_gen0(void)  { return ragged_matches(0); }
static test_result_t test_ragged_gen2(void)  { return ragged_matches(7); }
static test_result_t test_ragged_mixed(void) {
    test_result_t r = ragged_matches(3);          /* the ragged partition is generation 0 */
    if (r.status != TEST_PASS) return r;
    return ragged_matches(4);                     /* only the ragged partition is generation 2 */
}

/* The issue's repro: pinned answers, not only reference equality. */
static test_result_t test_issue_repro(void) {
    TEST_ASSERT_TRUE(write_db(PR_TMP "/repro", 0, true));
    ray_t* t = load_db(PR_TMP "/repro");
    TEST_ASSERT_NOT_NULL(t);
    bind_t(t);
    ray_t* r = ray_eval_str("(at (select {s: (sum b) from: t}) 's)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64 && r->len == 1);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[0], 150);
    drop_out(r);
    /* the four rows of 2024.01.03 group under the null key */
    r = ray_eval_str("(at (select {c: (count a) by: b from: t where: (> a 0)}) 'c)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64 && r->len == 6);
    int64_t total = 0, four = 0;
    for (int64_t i = 0; i < r->len; i++) { total += ((int64_t*)ray_data(r))[i]; four += ((int64_t*)ray_data(r))[i] == 4; }
    TEST_ASSERT_EQ_I(total, 9);
    TEST_ASSERT_EQ_I(four, 1);
    drop_out(r);
    r = ray_eval_str("(at (select {c: (count a) by: b from: t where: (> a 0)}) 'b)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64 && r->len == 6);
    int64_t nulls = 0;
    for (int64_t i = 0; i < r->len; i++) nulls += ((int64_t*)ray_data(r))[i] == NULL_I64;
    TEST_ASSERT_EQ_I(nulls, 1);
    drop_out(r);
    /* the filter keeps 2024.01.02 and 2024.01.03 only */
    r = ray_eval_str("(at (select {b: b from: t where: (>= date 2024.01.02)}) 'b)");
    TEST_ASSERT_TRUE(r && !RAY_IS_ERR(r) && r->type == RAY_I64);
    TEST_ASSERT_EQ_I(r->len, 6);
    static const int64_t want[6] = { 40, 50, NULL_I64, NULL_I64, NULL_I64, NULL_I64 };
    for (int i = 0; i < 6; i++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[i], want[i]);
    drop_out(r);
    bind_t(NULL);
    ray_release(t);
    PASS();
}

/* A loaded ragged column has a segment per partition, each with its
 * partition's MAPCOMMON row count; ray_parted_nrows is the table's rows. */
static test_result_t test_segments_full(void) {
    for (unsigned gen2 = 0; gen2 < 8; gen2 += 4) {
        char root[128]; snprintf(root, sizeof root, PR_TMP "/seg_%u", gen2);
        TEST_ASSERT_TRUE(write_db(root, gen2 | 1, true));
        ray_t* t = load_db(root);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_EQ_I(ray_table_nrows(t), 9);
        ray_t* mc = ray_table_col_idx(t, 0);
        TEST_ASSERT_TRUE(mc && mc->type == RAY_MAPCOMMON);
        const int64_t* counts = (const int64_t*)ray_data(((ray_t**)ray_data(mc))[1]);
        for (int64_t c = 1; c < ray_table_ncols(t); c++) {
            ray_t* slot = ray_table_get_col_enc_idx(t, c);
            ray_t* w = slot ? NULL : ray_table_col_idx(t, c);
            TEST_ASSERT_EQ_I(ray_table_col_nrows(slot ? slot : w), 9);
            if (w) TEST_ASSERT_EQ_I(ray_parted_nrows(w), 9);
            for (int32_t p = 0; p < 3; p++) {
                ray_t* seg = slot ? ray_enccol_seg(slot, p) : ((ray_t**)ray_data(w))[p];
                TEST_ASSERT_FMT(seg != NULL, "gen2 %u col %lld seg %d present", gen2, (long long)c, p);
                TEST_ASSERT_EQ_I(ray_table_col_nrows(seg), counts[p]);
            }
        }
        ray_release(t);
    }
    PASS();
}

/* A 2-row partition beside a 1M-row partition lacking b: every answer is
 * the null-filled reference's (a read past the 2-row b would not be). */
static test_result_t test_large_ragged(void) {
    for (unsigned gen2 = 0; gen2 < 4; gen2++) {
        char rr[128], rf[128], q[512];
        snprintf(rr, sizeof rr, PR_TMP "/lrag_%u", gen2);
        snprintf(rf, sizeof rf, PR_TMP "/lref_%u", gen2);
        for (int k = 0; k < 2; k++) {
            const char* root = k ? rf : rr;
            snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.01/t/\" (table [a b] (list [1 2] [10 20])) {compress: %s})",
                     root, (gen2 & 1) ? "true" : "false");
            TEST_ASSERT_TRUE(ev_ok(q));
            snprintf(q, sizeof q, "(.db.splayed.set \"%s/2024.01.02/t/\" %s {compress: %s})", root,
                     k ? "(table [a b] (list (til 1000000) (take [0Nl] 1000000)))" : "(table [a] (list (til 1000000)))",
                     (gen2 & 2) ? "true" : "false");
            TEST_ASSERT_TRUE(ev_ok(q));
        }
        ray_t* tr = load_db(rr);
        ray_t* tf = load_db(rf);
        TEST_ASSERT_TRUE(tr && tf);
        static const char* const lq[] = {
            "(select {s: (sum b) c: (count b) m: (max b) from: t})",
            "(select {c: (count a) by: b from: t})",
            "(select {s: (sum a) from: t where: (> b 0)})",
            "(select {b: b from: t where: (< a 3)})",
            "(select {s: (sum b) by: date from: t})",
            NULL,
        };
        for (int i = 0; lq[i]; i++) {
            bind_t(tf);
            ray_t* want = ray_eval_str(lq[i]);
            bind_t(tr);
            ray_t* got = ray_eval_str(lq[i]);
            bool same = same_value(want, got);
            drop_out(want); drop_out(got);
            TEST_ASSERT_FMT(same, "gen2 %u: %s", gen2, lq[i]);
        }
        bind_t(tr);
        ray_t* s = ray_eval_str("(at (select {s: (sum b) from: t}) 's)");
        TEST_ASSERT_TRUE(s && !RAY_IS_ERR(s) && s->type == RAY_I64 && s->len == 1);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(s))[0], 30);
        drop_out(s);
        bind_t(NULL);
        ray_release(tr); ray_release(tf);
    }
    PASS();
}

/* A hand-built parted column whose segments do not cover the table's rows
 * (here a missing segment, which ray_read_parted never produces) is refused
 * by the executor, never read past its end. */
static test_result_t test_short_column_refused(void) {
    TEST_ASSERT_TRUE(write_db(PR_TMP "/hand", 0, false));
    ray_t* src = load_db(PR_TMP "/hand");
    TEST_ASSERT_NOT_NULL(src);
    ray_t* b = ray_table_col(src, ray_sym_intern("b", 1));
    TEST_ASSERT_TRUE(b && RAY_IS_PARTED(b->type) && b->len == 3);
    ray_t* w = ray_alloc(3 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(w);
    w->type = b->type; w->len = 3; w->attrs = 0; memset(w->aux, 0, 16);
    ray_t** ws = (ray_t**)ray_data(w);
    for (int i = 0; i < 3; i++) {
        ws[i] = i == 2 ? NULL : ((ray_t**)ray_data(b))[i];
        if (ws[i]) ray_retain(ws[i]);
    }
    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_table_col_name(src, 0), ray_table_col_idx(src, 0));
    t = ray_table_add_col(t, ray_sym_intern("a", 1), ray_table_col(src, ray_sym_intern("a", 1)));
    t = ray_table_add_col(t, ray_sym_intern("b", 1), w);
    ray_release(w);
    TEST_ASSERT_TRUE(t && !RAY_IS_ERR(t));
    bind_t(t);
    static const char* const hq[] = {
        "(select {s: (sum b) from: t})",
        "(select {c: (count a) by: b from: t})",
        "(select {b: b from: t where: (>= date 2024.01.02)})",
        "(select {a: a b: b from: t})",
        NULL,
    };
    for (int i = 0; hq[i]; i++) {
        ray_t* r = ray_eval_str(hq[i]);
        bool err = r && RAY_IS_ERR(r);
        drop_out(r);
        TEST_ASSERT_FMT(err, "%s: refused", hq[i]);
    }
    bind_t(NULL);
    ray_release(t); ray_release(src);
    PASS();
}

const test_entry_t parted_ragged_entries[] = {
    { "parted_ragged/issue_repro",          test_issue_repro,          pr_setup, pr_teardown },
    { "parted_ragged/gen0",                 test_ragged_gen0,          pr_setup, pr_teardown },
    { "parted_ragged/gen2",                 test_ragged_gen2,          pr_setup, pr_teardown },
    { "parted_ragged/mixed",                test_ragged_mixed,         pr_setup, pr_teardown },
    { "parted_ragged/segments_full",        test_segments_full,        pr_setup, pr_teardown },
    { "parted_ragged/large",                test_large_ragged,         pr_setup, pr_teardown },
    { "parted_ragged/short_column_refused", test_short_column_refused, pr_setup, pr_teardown },
    { NULL, NULL, NULL, NULL },
};
