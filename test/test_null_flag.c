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

/* test/test_null_flag.c — HAS_NULLS agrees with the payload (#1167).
 *
 * Every kernel that writes a NULL sentinel into its output has to set
 * HAS_NULLS on it, and only then: a clear flag tells every reader there is
 * no null to look for.  Each test evaluates a group of kernels on inputs
 * that drive them onto the sentinel (float overflow, I64 wrap, empty
 * windows, missing join rows, null-bearing sources) and checks each result
 * with ray_test_nulls_agree, on three routes: eager, select and update.
 * The matrices run the query routes on 10,000 rows, which reaches the
 * morsel-parallel running scans and prefix windows; null_flag/parallel uses
 * 131,073 rows, past RAY_PARALLEL_THRESHOLD, so the task-parallel mvar,
 * mdev, mmin and mmax routes merge their workers' flags too. */
#include "test.h"
#include <rayforce.h>
#include "core/runtime.h"
#include "lang/eval.h"
#include <stdio.h>
#include <string.h>

static void nf_setup(void) {
    ray_runtime_create(0, NULL);
}

static void nf_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

/* Evaluate src; when it yields a vector, check its flag.  An error result
 * is reported through *err (a form may not accept every input type) and
 * counts as nothing checked. */
static bool nf_check(const char* src, int* checked, int* errs,
                     char* why, size_t cap) {
    ray_t* v = ray_eval_str(src);
    if (!v || RAY_IS_ERR(v)) {
        if (v) ray_error_free(v);
        (*errs)++;
        return true;
    }
    bool ok = true;
    if (ray_is_vec(v)) {
        (*checked)++;
        char inner[160];
        if (!ray_test_nulls_agree(v, inner, sizeof inner)) {
            snprintf(why, cap, "%s -> %s", src, inner);
            ok = false;
        }
    }
    ray_release(v);
    return ok;
}

static bool nf_eval_ok(const char* src) {
    ray_t* v = ray_eval_str(src);
    bool ok = v && !RAY_IS_ERR(v);
    if (v && RAY_IS_ERR(v)) ray_error_free(v);
    else if (v) ray_release(v);
    return ok;
}

/* Copy form into out with every '@' replaced by operand. */
static void nf_subst(char* out, size_t cap, const char* form, const char* operand) {
    size_t n = 0, ol = strlen(operand);
    for (const char* p = form; *p && n + 1 < cap; p++) {
        if (*p == '@') {
            size_t k = ol < cap - 1 - n ? ol : cap - 1 - n;
            memcpy(out + n, operand, k);
            n += k;
        } else {
            out[n++] = *p;
        }
    }
    out[n] = '\0';
}

typedef struct { const char* name; const char* lit; } nf_input_t;

/* The inputs of the #1167 sweep: overflow, domain edges and null-bearing
 * vectors in each numeric width. */
static const nf_input_t NF_INPUTS[] = {
    { "F64 over", "[1e308 1e308 1.0 2.0]" },
    { "F64 big",  "[1e200 1e200 3.0 4.0]" },
    { "F64 neg",  "[-1e308 -1e308 1.0 2.0]" },
    { "F64 null", "[1.0 0Nf 2.0 3.0]" },
    { "F64 zero", "[0.0 1.0 -1.0 2.0]" },
    { "I64 over", "[4611686018427387904 4611686018427387904 1 2]" },
    { "I64 max",  "[9223372036854775807 1 2 3]" },
    { "I64 min",  "[-9223372036854775807 -2 1 2]" },
    { "I64 null", "[1 0N 2 3]" },
    { "I64 zero", "[0 1 -1 2]" },
    { "I32 max",  "(as 'I32 [2147483647 1 2 3])" },
    { "I16 max",  "(as 'I16 [32767 1 2 3])" },
    { "U8 max",   "(as 'U8 [255 1 2 3])" },
    { "F32 over", "[3e38e 3e38e 1e 2e]" },
};
#define NF_NINPUTS ((int)(sizeof NF_INPUTS / sizeof NF_INPUTS[0]))

/* Run every form ('@' stands for the operand) over every
 * input on the three routes.  Fails on the first disagreement. */
static test_result_t nf_matrix(const char* const* forms, int nforms, int min_checked) {
    char src[1024], expr[512], why[1024];
    int checked = 0, errs = 0;
    for (int k = 0; k < NF_NINPUTS; k++) {
        const char* lit = NF_INPUTS[k].lit;
        snprintf(src, sizeof src,
                 "(set T (table [x] (list (take %s 10000))))", lit);
        TEST_ASSERT_FMT(nf_eval_ok(src), "setup %s", NF_INPUTS[k].name);
        for (int f = 0; f < nforms; f++) {
            nf_subst(src, sizeof src, forms[f], lit);
            if (!nf_check(src, &checked, &errs, why, sizeof why))
                FAILF("%s [eager]: %s", NF_INPUTS[k].name, why);
            nf_subst(expr, sizeof expr, forms[f], "x");
            snprintf(src, sizeof src, "(at (select {r: %s from: T}) 'r)", expr);
            if (!nf_check(src, &checked, &errs, why, sizeof why))
                FAILF("%s [select]: %s", NF_INPUTS[k].name, why);
            snprintf(src, sizeof src, "(at (update {r: %s from: T}) 'r)", expr);
            if (!nf_check(src, &checked, &errs, why, sizeof why))
                FAILF("%s [update]: %s", NF_INPUTS[k].name, why);
        }
    }
    /* Guard against a matrix that silently stops checking anything. */
    TEST_ASSERT_FMT(checked >= min_checked, "only %d vectors checked (%d errors)",
                    checked, errs);
    PASS();
}

/* Running scans: an F64 overflow is mapped to 0Nf, an I64 or TIMESPAN
 * total that wraps onto INT64_MIN is null. */
static test_result_t test_null_flag_running(void) {
    static const char* const forms[] = {
        "(sums @)", "(prds @)", "(avgs @)", "(mins @)", "(maxs @)",
    };
    return nf_matrix(forms, (int)(sizeof forms / sizeof forms[0]), 150);
}

/* Moving windows: msum/mavg on the prefix route, mvar/mdev on the
 * two-stack route, mmin/mmax on the deque route. */
static test_result_t test_null_flag_moving(void) {
    static const char* const forms[] = {
        "(msum 2 @)", "(msum 3 @)", "(mavg 2 @)", "(mavg 3 @)",
        "(mvar 2 @)", "(mvar 3 @)", "(mdev 2 @)", "(mdev 3 @)",
        "(mmin 2 @)", "(mmax 3 @)", "(mcount 2 @)",
    };
    return nf_matrix(forms, (int)(sizeof forms / sizeof forms[0]), 350);
}

/* Element-wise arithmetic and math, including the I64 wrap of #841. */
static test_result_t test_null_flag_elementwise(void) {
    static const char* const forms[] = {
        "(+ @ @)", "(* @ 2)", "(- 0 @)", "(/ @ 0)", "(% @ 0)",
        "(div @ 0)", "(neg @)", "(abs @)", "(sqrt @)", "(log @)",
        "(exp @)", "(reciprocal @)", "(deltas @)", "(ratios @)",
        "(pow @ 2)",
    };
    return nf_matrix(forms, (int)(sizeof forms / sizeof forms[0]), 400);
}

/* Casts between the numeric widths. */
static test_result_t test_null_flag_casts(void) {
    static const char* const forms[] = {
        "(as 'I16 @)", "(as 'I32 @)", "(as 'I64 @)", "(as 'F32 @)",
        "(as 'F64 @)", "(as 'TIMESTAMP @)",
    };
    return nf_matrix(forms, (int)(sizeof forms / sizeof forms[0]), 200);
}

/* Gathers and reorderings of a nullable source, and of a clean one: the
 * output carries a null exactly when it picked one. */
static test_result_t test_null_flag_gathers(void) {
    static const char* const forms[] = {
        "(reverse @)", "(asc @)", "(desc @)", "(distinct @)",
        "(take @ 2)", "(take @ -2)", "(take @ 9)", "(at @ [0 2])",
        "(at @ [1 3])", "(lag @)", "(lead @)", "(fills @)",
    };
    return nf_matrix(forms, (int)(sizeof forms / sizeof forms[0]), 300);
}

/* Joins: a left-join row with no match fills its value columns with nulls;
 * when every row matches there is nothing to flag. */
static test_result_t test_null_flag_joins(void) {
    static const char* const setup[] = {
        "(set L (table [k a] (list [1 2 3 4] [10 20 30 40])))",
        "(set R (table [k f i t] (list [1 2] [1.5 2.5] [7 8] [2024.01.01D00:00:00.000000000 2024.01.02D00:00:00.000000000])))",
        "(set Rall (table [k f i] (list [1 2 3 4] [1.5 2.5 3.5 4.5] [7 8 9 10])))",
        NULL
    };
    for (int q = 0; setup[q]; q++)
        TEST_ASSERT_FMT(nf_eval_ok(setup[q]), "setup %d", q);
    static const char* const exprs[] = {
        "(at (left-join [k] L R) 'f)",
        "(at (left-join [k] L R) 'i)",
        "(at (left-join [k] L R) 't)",
        "(at (left-join [k] L Rall) 'f)",
        "(at (left-join [k] L Rall) 'i)",
        "(at (inner-join [k] L R) 'f)",
        NULL
    };
    char why[1024];
    int checked = 0, errs = 0;
    for (int q = 0; exprs[q]; q++)
        if (!nf_check(exprs[q], &checked, &errs, why, sizeof why))
            FAILF("%s", why);
    TEST_ASSERT_FMT(errs == 0, "%d join forms failed to evaluate", errs);
    PASS();
}

/* Past RAY_PARALLEL_THRESHOLD every window kernel splits the rows over
 * several tasks.  Each input puts its only nulls in the last rows (one
 * worker writes them) or the first rows (empty windows), or has none at all,
 * so a flag merged from the wrong worker or set without a null shows up. */
static test_result_t test_null_flag_parallel(void) {
    static const char* const setup[] = {
        "(set N 131071)",
        "(set Fend (concat (take 0.0 N) [1e308 1e308]))",
        "(set Iend (concat (take 0 N) [4611686018427387904 4611686018427387904]))",
        "(set Fhead (concat (take 0Nf N) [1.0 2.0]))",
        "(set Ihead (concat (take 0N N) [1 2]))",
        "(set Fclean (as 'F64 (til (+ N 2))))",
        "(set T (table [fe ie fh ih fc] (list Fend Iend Fhead Ihead Fclean)))",
        NULL
    };
    for (int q = 0; setup[q]; q++)
        TEST_ASSERT_FMT(nf_eval_ok(setup[q]), "setup %d", q);
    static const char* const forms[] = {
        "(sums @)", "(prds @)", "(avgs @)", "(mins @)", "(maxs @)",
        "(msum 2 @)", "(mavg 1000 @)", "(mvar 2 @)", "(mdev 1000 @)",
        "(mmin 2 @)", "(mmax 1000 @)",
    };
    static const char* const vecs[][2] = {
        { "Fend", "fe" }, { "Iend", "ie" }, { "Fhead", "fh" },
        { "Ihead", "ih" }, { "Fclean", "fc" },
    };
    char src[512], expr[256], why[1024];
    int checked = 0, errs = 0;
    for (size_t v = 0; v < sizeof vecs / sizeof vecs[0]; v++) {
        for (size_t f = 0; f < sizeof forms / sizeof forms[0]; f++) {
            nf_subst(src, sizeof src, forms[f], vecs[v][0]);
            if (!nf_check(src, &checked, &errs, why, sizeof why))
                FAILF("[eager]: %s", why);
            nf_subst(expr, sizeof expr, forms[f], vecs[v][1]);
            snprintf(src, sizeof src, "(at (select {r: %s from: T}) 'r)", expr);
            if (!nf_check(src, &checked, &errs, why, sizeof why))
                FAILF("[select]: %s", why);
        }
    }
    TEST_ASSERT_FMT(checked >= 100, "only %d vectors checked (%d errors)",
                    checked, errs);
    PASS();
}

const test_entry_t null_flag_entries[] = {
    { "null_flag/parallel",    test_null_flag_parallel,    nf_setup, nf_teardown },
    { "null_flag/running",     test_null_flag_running,     nf_setup, nf_teardown },
    { "null_flag/moving",      test_null_flag_moving,      nf_setup, nf_teardown },
    { "null_flag/elementwise", test_null_flag_elementwise, nf_setup, nf_teardown },
    { "null_flag/casts",       test_null_flag_casts,       nf_setup, nf_teardown },
    { "null_flag/gathers",     test_null_flag_gathers,     nf_setup, nf_teardown },
    { "null_flag/joins",       test_null_flag_joins,       nf_setup, nf_teardown },
    { NULL, NULL, NULL, NULL },
};
