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

/* test/test_join_mask.c — #596 semi-mask / anti-mask.
 *
 * Oracle: the left row ids the inner join emits, run with the prefilter
 * off (the plain probe).  A left table carries rid = its row number, so
 * (at (inner-join keys L R) 'rid) names exactly the rows the semi mask must
 * set.  Every case also checks anti-mask is the negation, and that
 * selecting L by the anti mask gives the anti-join's rows in order.  The
 * prefilter counters say whether the mask took the bitmap path, so a case
 * that expects it cannot pass by silently probing every row. */

#include "test.h"
#include "rayforce.h"
#include "ops/ops.h"
#include "ops/internal.h"
#include "lang/internal.h"
#include "lang/env.h"
#include "table/sym.h"
#include "core/pool.h"
#include "mem/heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JM_TMP "/tmp/rayforce_test_join_mask"

static void jm_setup(void) {
    ray_runtime_create(0, NULL);
    (void)ray_test_rm_rf(ray_test_tmp(JM_TMP));
    (void)ray_test_mkdir_p(ray_test_tmp(JM_TMP));
}
static void jm_teardown(void) {
    ray_join_no_prefilter = false;
    (void)ray_test_rm_rf(ray_test_tmp(JM_TMP));
    ray_runtime_destroy(__RUNTIME);
}

/* ── Fixtures ─────────────────────────────────────────────────────────────
 * [venue instrument sid tag] + rid (left) or w (right).  venue/instrument
 * are SYM at the given widths, sid an integer of type sid_t, tag F64.
 * Keys draw from nv venues, ni instruments, ns sids; each key cell is null
 * with probability 1/null_1in (0 = never). */
typedef struct {
    int64_t  n, nv, ni, ns, null_1in;
    uint8_t  wv, wi;
    int8_t   sid_t;
    bool     left;
    uint64_t seed;
} jm_spec_t;

static uint64_t jm_rng(uint64_t* s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s;
}

static int64_t jm_sym(const char* p, int64_t i) {
    char b[32]; snprintf(b, sizeof b, "%s%lld", p, (long long)i);
    return ray_sym_intern(b, strlen(b));
}

static ray_t* jm_add(ray_t* tbl, const char* name, ray_t* col) {
    tbl = ray_table_add_col(tbl, ray_sym_intern(name, strlen(name)), col);
    ray_release(col);
    return tbl;
}

static ray_t* jm_table(const jm_spec_t* sp) {
    int64_t n = sp->n;
    uint64_t s = sp->seed | 1;
    ray_t* v = ray_sym_vec_new(sp->wv, n);
    ray_t* in = ray_sym_vec_new(sp->wi, n);
    ray_t* sid = ray_vec_new(sp->sid_t, n);
    ray_t* tag = ray_vec_new(RAY_F64, n);
    ray_t* id = ray_vec_new(RAY_I64, n);
    v->len = in->len = sid->len = tag->len = id->len = n;
    for (int64_t r = 0; r < n; r++) {
        bool nv = sp->null_1in && jm_rng(&s) % (uint64_t)sp->null_1in == 0;
        bool ni = sp->null_1in && jm_rng(&s) % (uint64_t)sp->null_1in == 0;
        bool ns = sp->null_1in && jm_rng(&s) % (uint64_t)sp->null_1in == 0;
        int64_t iv = (int64_t)(jm_rng(&s) % (uint64_t)sp->nv);
        int64_t ii = (int64_t)(jm_rng(&s) % (uint64_t)sp->ni);
        int64_t is = (int64_t)(jm_rng(&s) % (uint64_t)sp->ns);
        ray_write_sym(ray_data(v), r, nv ? 0 : (uint64_t)jm_sym("V", iv), RAY_SYM, v->attrs);
        ray_write_sym(ray_data(in), r, ni ? 0 : (uint64_t)jm_sym("INST", ii), RAY_SYM, in->attrs);
        switch (sp->sid_t) {
        case RAY_I16: ((int16_t*)ray_data(sid))[r] = (int16_t)is; break;
        case RAY_I32: ((int32_t*)ray_data(sid))[r] = (int32_t)is; break;
        default:      ((int64_t*)ray_data(sid))[r] = is; break;
        }
        if (ns) ray_vec_set_null(sid, r, true);
        ((double*)ray_data(tag))[r] = (double)(ii % 7) * 0.5;
        ((int64_t*)ray_data(id))[r] = sp->left ? r : (int64_t)(jm_rng(&s) % 1000);
    }
    ray_t* t = ray_table_new(5);
    t = jm_add(t, "venue", v);
    t = jm_add(t, "instrument", in);
    t = jm_add(t, "sid", sid);
    t = jm_add(t, "tag", tag);
    t = jm_add(t, sp->left ? "rid" : "w", id);
    return t;
}

static void jm_bind(const char* name, ray_t* tbl) {
    (void)ray_env_set(ray_sym_intern(name, strlen(name)), tbl);
    ray_release(tbl);
}

static ray_t* jm_eval(const char* fmt, const char* a, const char* b, const char* c) {
    char src[512];
    snprintf(src, sizeof src, fmt, a, b, c);
    return ray_eval_str(src);
}

/* ── The check ────────────────────────────────────────────────────────────
 * keys like "[venue instrument]".  expect_pf: 1 = the mask must build the
 * prefilter and skip rows, 0 = it must not build it, -1 = either.
 * *set_out gets the number of rows the semi mask sets. */
static test_result_t jm_check(const char* keys, const char* L, const char* R,
                              int expect_pf, int64_t* set_out) {
    ray_join_no_prefilter = true;
    ray_t* ids = jm_eval("(at (inner-join %s %s %s) 'rid)", keys, L, R);
    ray_join_no_prefilter = false;
    TEST_ASSERT(ids && !RAY_IS_ERR(ids), "oracle inner join");

    uint64_t b0 = ray_join_prefilter_builds;
    uint64_t s0 = atomic_load(&ray_join_prefilter_skips);
    ray_t* semi = jm_eval("(semi-mask %s %s %s)", keys, L, R);
    bool built = ray_join_prefilter_builds > b0;
    bool skipped = atomic_load(&ray_join_prefilter_skips) > s0;
    ray_t* anti = jm_eval("(anti-mask %s %s %s)", keys, L, R);
    TEST_ASSERT(semi && !RAY_IS_ERR(semi), "semi-mask");
    TEST_ASSERT(anti && !RAY_IS_ERR(anti), "anti-mask");

    ray_t* lt = ray_env_get(ray_sym_intern(L, strlen(L)));
    int64_t nl = ray_table_nrows(lt);
    TEST_ASSERT_EQ_I(semi->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(anti->type, RAY_BOOL);
    TEST_ASSERT_EQ_I(ray_len(semi), nl);
    TEST_ASSERT_EQ_I(ray_len(anti), nl);

    uint8_t* want = calloc((size_t)(nl ? nl : 1), 1);
    const int64_t* id = (const int64_t*)ray_data(ids);
    for (int64_t i = 0; i < ray_len(ids); i++) want[id[i]] = 1;
    const uint8_t* sm = (const uint8_t*)ray_data(semi);
    const uint8_t* am = (const uint8_t*)ray_data(anti);
    int64_t bad = -1, set = 0;
    for (int64_t r = 0; r < nl && bad < 0; r++) {
        if (sm[r] != want[r] || am[r] != !want[r]) bad = r;
        set += want[r];
    }
    free(want);
    ray_release(ids);
    if (bad >= 0) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                 "mask mismatch at left row %lld: (semi-mask %s %s %s)",
                 (long long)bad, keys, L, R);
        ray_release(semi); ray_release(anti);
        return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }

    /* Selecting by the anti mask = the anti-join, rows in order. */
    (void)ray_env_set(ray_sym_intern("JM_AM", 5), anti);
    ray_release(semi); ray_release(anti);
    ray_t* aj = jm_eval("(at (anti-join %s %s %s) 'rid)", keys, L, R);
    char src[256];
    snprintf(src, sizeof src, "(at (select {from: %s where: JM_AM}) 'rid)", L);
    ray_t* sel = ray_eval_str(src);
    TEST_ASSERT(aj && !RAY_IS_ERR(aj), "anti-join");
    TEST_ASSERT(sel && !RAY_IS_ERR(sel), "select by anti mask");
    TEST_ASSERT_EQ_I(ray_len(aj), ray_len(sel));
    TEST_ASSERT_EQ_I(ray_len(aj), nl - set);
    TEST_ASSERT(ray_len(aj) == 0 ||
                memcmp(ray_data(aj), ray_data(sel), (size_t)ray_len(aj) * 8) == 0,
                "select by anti mask matches the anti-join");
    ray_release(aj); ray_release(sel);

    if (expect_pf == 1 && !(built && skipped)) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                 "prefilter expected to build and skip rows: (semi-mask %s %s %s)", keys, L, R);
        return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }
    if (expect_pf == 0 && built) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                 "prefilter built where it cannot apply: (semi-mask %s %s %s)", keys, L, R);
        return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }
    if (set_out) *set_out = set;
    PASS();
}

#define JM_CHECK(keys, L, R, pf, setv)                                         \
    do {                                                                       \
        test_result_t _r = jm_check((keys), (L), (R), (pf), &(setv));          \
        if (_r.status != TEST_PASS) return _r;                                 \
    } while (0)

/* ── Tests ────────────────────────────────────────────────────────────── */

/* The issue's shape: a large left, a few books on the right, compound SYM
 * keys at mixed widths, nulls on both sides.  Prefiltered. */
static test_result_t test_jm_big_left_few_books(void) {
    jm_bind("L", jm_table(&(jm_spec_t){ .n = 50000, .nv = 6, .ni = 400, .ns = 5,
        .null_1in = 17, .wv = RAY_SYM_W16, .wi = RAY_SYM_W32, .sid_t = RAY_I64,
        .left = true, .seed = 11 }));
    jm_bind("R", jm_table(&(jm_spec_t){ .n = 40, .nv = 6, .ni = 400, .ns = 5,
        .null_1in = 9, .wv = RAY_SYM_W64, .wi = RAY_SYM_W16, .sid_t = RAY_I32,
        .left = false, .seed = 12 }));
    int64_t set = 0;
    JM_CHECK("[venue instrument]", "L", "R", 1, set);
    TEST_ASSERT(set > 0 && set < 50000 / 4, "few books match a few rows");
    JM_CHECK("[venue instrument sid]", "L", "R", 1, set);
    JM_CHECK("[instrument]", "L", "R", 1, set);
    JM_CHECK("[sid instrument]", "L", "R", 1, set);
    /* key order and argument order: keys last, like the joins accept */
    ray_t* a = ray_eval_str("(semi-mask [venue instrument] L R)");
    ray_t* b = ray_eval_str("(semi-mask L R [venue instrument])");
    TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "both conventions");
    TEST_ASSERT_EQ_I(ray_len(a), ray_len(b));
    TEST_ASSERT(memcmp(ray_data(a), ray_data(b), (size_t)ray_len(a)) == 0, "same mask");
    ray_release(a); ray_release(b);
    PASS();
}

/* Random shapes over many seeds: domains from tiny to wide, nulls dense to
 * none, left larger and smaller than right, single and compound keys. */
static test_result_t test_jm_random(void) {
    static const uint8_t widths[] = { RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    static const int8_t sids[] = { RAY_I16, RAY_I32, RAY_I64 };
    static const char* keysets[] = { "[venue]", "[sid]",
        "[venue instrument]", "[venue instrument sid]", "[venue tag]" };
    uint64_t s = 0x5eed;
    for (int it = 0; it < 14; it++) {
        int64_t nl = 1 + (int64_t)(jm_rng(&s) % 6000);
        int64_t nr = (int64_t)(jm_rng(&s) % 3 == 0 ? jm_rng(&s) % 8000 : jm_rng(&s) % 60);
        int64_t ni = 1 + (int64_t)(jm_rng(&s) % 300);
        int64_t nv = 1 + (int64_t)(jm_rng(&s) % 5);
        int64_t ns = 1 + (int64_t)(jm_rng(&s) % 9);
        int64_t nulls = (int64_t)(jm_rng(&s) % 4) * 5;     /* 0 = none */
        uint8_t wl = widths[jm_rng(&s) % 3], wr = widths[jm_rng(&s) % 3];
        jm_bind("L", jm_table(&(jm_spec_t){ .n = nl, .nv = nv, .ni = ni, .ns = ns,
            .null_1in = nulls, .wv = wl, .wi = wr, .sid_t = sids[jm_rng(&s) % 3],
            .left = true, .seed = jm_rng(&s) }));
        jm_bind("R", jm_table(&(jm_spec_t){ .n = nr, .nv = nv, .ni = ni + 3, .ns = ns,
            .null_1in = nulls ? nulls + 1 : 0, .wv = wr, .wi = wl,
            .sid_t = sids[jm_rng(&s) % 3], .left = false, .seed = jm_rng(&s) }));
        /* distinct values per keyset, to keep the oracle join's output small */
        int64_t card[] = { nv, ns, nv * ni, nv * ni * ns, nv * 7 };
        for (size_t k = 0; k < sizeof keysets / sizeof *keysets; k++) {
            if (nl * nr > 20000 * card[k]) continue;
            int64_t set = 0;
            JM_CHECK(keysets[k], "L", "R", -1, set);
        }
    }
    PASS();
}

/* Null keys: null equals null, as in the joins.  A left row whose key is
 * all null is set iff the right side holds an all-null key. */
static test_result_t test_jm_null_keys(void) {
    ray_t* l = ray_eval_str(
        "(table [k j rid] (list (as 'SYM [\"a\" \"\" \"b\" \"\" \"c\"]) [1 0Nl 2 0Nl 3] [0 1 2 3 4]))");
    TEST_ASSERT(l && !RAY_IS_ERR(l), "left");
    jm_bind("L", l);
    ray_t* r = ray_eval_str("(table [k j] (list (as 'SYM [\"\" \"c\"]) [0Nl 3]))");
    TEST_ASSERT(r && !RAY_IS_ERR(r), "right");
    jm_bind("R", r);
    ray_t* r2 = ray_eval_str("(table [k j] (list (as 'SYM [\"a\" \"c\"]) [1 3]))");
    jm_bind("R2", r2);
    int64_t set = 0;
    JM_CHECK("[k]", "L", "R", -1, set);
    TEST_ASSERT_EQ_I(set, 3);           /* both nulls and c */
    JM_CHECK("[k j]", "L", "R", -1, set);
    TEST_ASSERT_EQ_I(set, 3);
    JM_CHECK("[j]", "L", "R2", -1, set);
    TEST_ASSERT_EQ_I(set, 2);           /* no null on the right */
    /* the self mask is all true, nulls included */
    ray_t* m = ray_eval_str("(sum (semi-mask [k j] L L))");
    TEST_ASSERT(m && !RAY_IS_ERR(m), "self mask");
    TEST_ASSERT_EQ_I(m->i64, 5);
    ray_release(m);
    PASS();
}

/* Empty sides: an empty right sets nothing (anti: everything); an empty
 * left gives an empty BOOL vector. */
static test_result_t test_jm_empty(void) {
    jm_bind("L", jm_table(&(jm_spec_t){ .n = 3000, .nv = 3, .ni = 50, .ns = 3,
        .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64, .left = true, .seed = 3 }));
    jm_bind("R", jm_table(&(jm_spec_t){ .n = 0, .nv = 3, .ni = 50, .ns = 3,
        .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64, .left = false, .seed = 4 }));
    jm_bind("E", jm_table(&(jm_spec_t){ .n = 0, .nv = 3, .ni = 50, .ns = 3,
        .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64, .left = true, .seed = 5 }));
    int64_t set = 0;
    JM_CHECK("[venue instrument]", "L", "R", 0, set);
    TEST_ASSERT_EQ_I(set, 0);
    JM_CHECK("[venue instrument]", "E", "L", 0, set);
    TEST_ASSERT_EQ_I(set, 0);
    JM_CHECK("[venue instrument]", "E", "R", 0, set);
    ray_t* a = ray_eval_str("(anti-mask [venue] L R)");
    TEST_ASSERT(a && !RAY_IS_ERR(a), "anti over empty right");
    for (int64_t i = 0; i < ray_len(a); i++)
        TEST_ASSERT_EQ_I(((uint8_t*)ray_data(a))[i], 1);
    ray_release(a);
    PASS();
}

/* Parallel paths: a left large enough to split across the pool, once with
 * a few books (prefiltered probe) and once against a right side larger
 * than the parallel threshold (parallel build, plain probe). */
static test_result_t test_jm_large(void) {
    jm_bind("L", jm_table(&(jm_spec_t){ .n = 200000, .nv = 6, .ni = 800, .ns = 4,
        .null_1in = 101, .wv = RAY_SYM_W16, .wi = RAY_SYM_W16, .sid_t = RAY_I32,
        .left = true, .seed = 21 }));
    jm_bind("R", jm_table(&(jm_spec_t){ .n = 25, .nv = 6, .ni = 800, .ns = 4,
        .null_1in = 0, .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64,
        .left = false, .seed = 22 }));
    jm_bind("B", jm_table(&(jm_spec_t){ .n = 80000, .nv = 6, .ni = 1600, .ns = 4,
        .null_1in = 97, .wv = RAY_SYM_W32, .wi = RAY_SYM_W32, .sid_t = RAY_I64,
        .left = false, .seed = 23 }));
    int64_t set = 0;
    JM_CHECK("[venue instrument]", "L", "R", 1, set);
    TEST_ASSERT(set > 0, "books match");
    JM_CHECK("[venue instrument sid]", "L", "R", 1, set);
    JM_CHECK("[venue instrument sid]", "L", "B", 0, set);
    TEST_ASSERT(set > 0 && set < 200000, "partial match against the big right");
    /* the skip count is exact: every rejected row is counted once */
    uint64_t s0 = atomic_load(&ray_join_prefilter_skips);
    ray_t* m = ray_eval_str("(semi-mask [instrument] L R)");
    TEST_ASSERT(m && !RAY_IS_ERR(m), "mask");
    int64_t ones = 0;
    for (int64_t i = 0; i < ray_len(m); i++) ones += ((uint8_t*)ray_data(m))[i];
    TEST_ASSERT((int64_t)(atomic_load(&ray_join_prefilter_skips) - s0) <= 200000 - ones,
                "skips never exceed the unmatched rows");
    TEST_ASSERT((int64_t)(atomic_load(&ray_join_prefilter_skips) - s0) > (200000 - ones) / 2,
                "most unmatched rows are skipped");
    ray_release(m);
    PASS();
}

/* Cross-domain SYM keys: a splayed (FILE-domain, W8) left against
 * runtime-domain right keys, and both sides from one symfile. */
static test_result_t test_jm_file_domain(void) {
    jm_bind("L0", jm_table(&(jm_spec_t){ .n = 20000, .nv = 5, .ni = 120, .ns = 3,
        .null_1in = 13, .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64,
        .left = true, .seed = 31 }));
    jm_bind("R", jm_table(&(jm_spec_t){ .n = 12, .nv = 5, .ni = 130, .ns = 3,
        .null_1in = 4, .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64,
        .left = false, .seed = 32 }));
    ray_t* e = ray_eval_str(ray_test_tmp(
        "(.db.splayed.set \"" JM_TMP "/l/\" L0 \"" JM_TMP "/.sym\")"));
    TEST_ASSERT(e && !RAY_IS_ERR(e), "save left");
    ray_release(e);
    e = ray_eval_str(ray_test_tmp(
        "(.db.splayed.set \"" JM_TMP "/r/\" R \"" JM_TMP "/.sym\")"));
    TEST_ASSERT(e && !RAY_IS_ERR(e), "save right");
    ray_release(e);
    ray_t* lf = ray_eval_str(ray_test_tmp(
        "(.db.splayed.get \"" JM_TMP "/l/\" \"" JM_TMP "/.sym\")"));
    TEST_ASSERT(lf && !RAY_IS_ERR(lf), "load left");
    ray_t* rf = ray_eval_str(ray_test_tmp(
        "(.db.splayed.get \"" JM_TMP "/r/\" \"" JM_TMP "/.sym\")"));
    TEST_ASSERT(rf && !RAY_IS_ERR(rf), "load right");
    ray_t* lv = ray_table_col(lf, ray_sym_intern("instrument", 10));
    TEST_ASSERT(lv && ray_sym_vec_domain(lv) != ray_sym_runtime_domain(),
                "splayed SYM key is in a file domain");
    jm_bind("LF", lf);
    jm_bind("RF", rf);
    int64_t set = 0, set_f = 0;
    JM_CHECK("[venue instrument]", "LF", "R", 1, set);
    JM_CHECK("[venue instrument]", "LF", "RF", 1, set_f);
    TEST_ASSERT_EQ_I(set, set_f);
    JM_CHECK("[venue instrument sid]", "LF", "R", 1, set);
    PASS();
}

/* Key types outside the prefilter: STR, F64 and GUID keys take the hash
 * probe alone and still agree with the inner join. */
static test_result_t test_jm_unfiltered_types(void) {
    jm_bind("L", jm_table(&(jm_spec_t){ .n = 9000, .nv = 4, .ni = 90, .ns = 3,
        .null_1in = 23, .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64,
        .left = true, .seed = 41 }));
    jm_bind("R", jm_table(&(jm_spec_t){ .n = 10, .nv = 4, .ni = 90, .ns = 3,
        .null_1in = 5, .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64,
        .left = false, .seed = 42 }));
    ray_t* e = ray_eval_str(
        "(do (set LS (table [s v rid] (list (as 'STR (at L 'instrument)) (at L 'venue) (at L 'rid))))"
        "    (set RS (table [s v] (list (as 'STR (at R 'instrument)) (at R 'venue)))) 0)");
    TEST_ASSERT(e && !RAY_IS_ERR(e), "STR tables");
    ray_release(e);
    int64_t set = 0;
    JM_CHECK("[tag]", "L", "R", 0, set);
    JM_CHECK("[s]", "LS", "RS", 0, set);
    TEST_ASSERT(set > 0, "STR keys match");
    JM_CHECK("[s v]", "LS", "RS", -1, set);
    PASS();
}

/* A mask feeds where: directly, staged or inline, and delete. */
static test_result_t test_jm_where(void) {
    ray_t* r = ray_eval_str(
        "(do (set T (table [k v rid] (list (as 'SYM [\"a\" \"b\" \"c\" \"a\"]) [1 2 3 4] [0 1 2 3])))"
        "    (set K (table [k] (list (as 'SYM [\"a\" \"z\"]))))"
        "    (set M (semi-mask [k] T K))"
        "    (list (at (select {from: T where: M}) 'rid)"
        "          (at (select {from: T where: (anti-mask [k] T K)}) 'rid)"
        "          (count (delete {from: T where: M}))))");
    TEST_ASSERT(r && !RAY_IS_ERR(r), "where composition");
    TEST_ASSERT_EQ_I(ray_len(r), 3);
    ray_t** e = (ray_t**)ray_data(r);
    TEST_ASSERT_EQ_I(ray_len(e[0]), 2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(e[0]))[0], 0);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(e[0]))[1], 3);
    TEST_ASSERT_EQ_I(ray_len(e[1]), 2);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(e[1]))[0], 1);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(e[1]))[1], 2);
    TEST_ASSERT_EQ_I(e[2]->i64, 2);
    ray_release(r);
    PASS();
}

/* Argument errors name the builtin. */
static test_result_t test_jm_errors(void) {
    ray_t* e = ray_eval_str(
        "(do (set T (table [k s] (list [1 2] (list \"x\" \"y\"))))"
        "    (set U (table [k s] (list [1 3] (as 'SYM [\"x\" \"z\"])))) 0)");
    TEST_ASSERT(e && !RAY_IS_ERR(e), "fixtures");
    ray_release(e);
    static const char* bad[] = {
        "(semi-mask [k] T)",
        "(semi-mask [k] T 1)",
        "(semi-mask [] T U)",
        "(semi-mask [q] T U)",
        "(anti-mask [1] T U)",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        ray_t* r = ray_eval_str(bad[i]);
        TEST_ASSERT(r && RAY_IS_ERR(r), bad[i]);
        ray_release(r);
    }
    PASS();
}

/* Cancellation.  The mask dispatches the build (a right side past the
 * parallel threshold) and the probe; a cancel that lands before either
 * leaves its buffers unwritten, over blocks scribbled with garbage
 * (ray_heap_test_scribble).  The query runs once per dispatch with the
 * cancel armed right before it (ray_pool_test_cancel_at): each run is
 * `cancel` or the answer, never a mask read out of a skipped probe.  Mode
 * 0 is a cancel request (both flags), until the run armed past the last
 * dispatch answers; mode 1 the pool's flag alone, under which every one of
 * those dispatches answers `cancel`. */
static test_result_t jm_cancel_case(const char* q, int min_dispatches) {
    ray_t* want = ray_eval_str(q);
    TEST_ASSERT(want && !RAY_IS_ERR(want), q);
    int64_t n = ray_len(want);
    ray_heap_test_scribble(true);
    int last = 0, cancelled = 0;
    const char* fail = NULL;
    for (int mode = 0; mode < 2 && !fail; mode++) {
        for (int k = 1; k <= 16 && (mode == 0 || k <= last) && !fail; k++) {
            ray_pool_test_cancel_at((uint32_t)k, mode == 1);
            ray_t* r = ray_eval_str(q);
            ray_pool_test_cancel_at(0, false);
            ray_clear_interrupt();
            if (r && RAY_IS_ERR(r)) {
                if (strcmp(ray_err_code(r), "cancel") != 0) fail = "an error other than cancel";
                cancelled++;
                ray_release(r);
                continue;
            }
            if (mode == 1) fail = "answered under the pool's cancel";
            else if (!r || r->type != RAY_BOOL || ray_len(r) != n ||
                     memcmp(ray_data(r), ray_data(want), (size_t)n) != 0)
                fail = "a wrong mask after a cancel";
            else last = k - 1;
            if (r) ray_release(r);
            break;
        }
    }
    ray_heap_test_scribble(false);
    ray_release(want);
    if (!fail && last < min_dispatches) fail = "fewer dispatches than expected";
    if (!fail && cancelled != 2 * last) fail = "a run neither cancelled nor answered";
    if (fail) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf, "%s: %s (%d dispatches)",
                 q, fail, last);
        return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }
    PASS();
}

static test_result_t test_jm_cancel_at_dispatch(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    jm_bind("L", jm_table(&(jm_spec_t){ .n = 200000, .nv = 6, .ni = 800, .ns = 4,
        .null_1in = 101, .wv = RAY_SYM_W16, .wi = RAY_SYM_W16, .sid_t = RAY_I32,
        .left = true, .seed = 51 }));
    jm_bind("R", jm_table(&(jm_spec_t){ .n = 25, .nv = 6, .ni = 800, .ns = 4,
        .null_1in = 0, .wv = RAY_SYM_W64, .wi = RAY_SYM_W64, .sid_t = RAY_I64,
        .left = false, .seed = 52 }));
    jm_bind("B", jm_table(&(jm_spec_t){ .n = 80000, .nv = 6, .ni = 1600, .ns = 4,
        .null_1in = 97, .wv = RAY_SYM_W32, .wi = RAY_SYM_W32, .sid_t = RAY_I64,
        .left = false, .seed = 53 }));
    static const struct { const char* q; int d; } cases[] = {
        { "(semi-mask [venue instrument sid] L B)", 2 },   /* build + lookahead probe */
        { "(anti-mask [venue instrument sid] L B)", 2 },
        { "(semi-mask [venue instrument] L R)", 1 },       /* prefiltered probe */
        { "(anti-mask [venue instrument] L R)", 1 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        test_result_t r = jm_cancel_case(cases[i].q, cases[i].d);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}

/* A table whose only special column is MAPCOMMON counts as parted but has
 * no segments: it flattens to nothing, which is an error, as in the joins,
 * not a retry with the same table. */
static test_result_t test_jm_mapcommon_only(void) {
    ray_t* keys = ray_vec_new(RAY_I64, 2); keys->len = 2;
    ray_t* counts = ray_vec_new(RAY_I64, 2); counts->len = 2;
    ((int64_t*)ray_data(keys))[0] = 1; ((int64_t*)ray_data(keys))[1] = 2;
    ((int64_t*)ray_data(counts))[0] = 3; ((int64_t*)ray_data(counts))[1] = 4;
    ray_t* mc = ray_alloc(2 * sizeof(ray_t*));
    TEST_ASSERT_NOT_NULL(mc);
    mc->type = RAY_MAPCOMMON; mc->len = 2;
    ((ray_t**)ray_data(mc))[0] = keys; ((ray_t**)ray_data(mc))[1] = counts;
    int64_t k = ray_sym_intern("k", 1);
    ray_t* mt = ray_table_add_col(ray_table_new(1), k, mc);
    ray_release(mc);
    ray_t* kv = ray_vec_new(RAY_I64, 3); kv->len = 3;
    for (int64_t i = 0; i < 3; i++) ((int64_t*)ray_data(kv))[i] = i;
    ray_t* ft = ray_table_add_col(ray_table_new(1), k, kv);
    ray_release(kv);
    TEST_ASSERT(mt && !RAY_IS_ERR(mt) && ft && !RAY_IS_ERR(ft), "tables");
    for (int anti = 0; anti < 2; anti++) {
        ray_t* a = ray_join_mask(mt, ft, &k, 1, anti);
        ray_t* b = ray_join_mask(ft, mt, &k, 1, anti);
        bool ok = a && RAY_IS_ERR(a) && b && RAY_IS_ERR(b);
        if (a) ray_release(a);
        if (b) ray_release(b);
        TEST_ASSERT(ok, "a MAPCOMMON-only side is an error");
    }
    ray_release(mt); ray_release(ft);
    PASS();
}

const test_entry_t join_mask_entries[] = {
    { "join_mask/big_left_few_books", test_jm_big_left_few_books, jm_setup, jm_teardown },
    { "join_mask/random",             test_jm_random,             jm_setup, jm_teardown },
    { "join_mask/null_keys",          test_jm_null_keys,          jm_setup, jm_teardown },
    { "join_mask/empty",              test_jm_empty,              jm_setup, jm_teardown },
    { "join_mask/large",              test_jm_large,              jm_setup, jm_teardown },
    { "join_mask/file_domain",        test_jm_file_domain,        jm_setup, jm_teardown },
    { "join_mask/unfiltered_types",   test_jm_unfiltered_types,   jm_setup, jm_teardown },
    { "join_mask/where",              test_jm_where,              jm_setup, jm_teardown },
    { "join_mask/errors",             test_jm_errors,             jm_setup, jm_teardown },
    { "join_mask/cancel_at_dispatch", test_jm_cancel_at_dispatch, jm_setup, jm_teardown },
    { "join_mask/mapcommon_only",     test_jm_mapcommon_only,     jm_setup, jm_teardown },
    { NULL, NULL, NULL, NULL },
};
