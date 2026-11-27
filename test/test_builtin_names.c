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

/* Two public names that reach one function are two ways to spell one thing:
 * a second spelling a user must learn, document and keep in step, as the
 * planner's `median` was (#1814).  Every registered builtin is enumerated
 * here, namespace members included (.csv.read is the member read of .csv),
 * and two names holding one function object fail the test unless the pair
 * is listed below with a reason.  A listed pair that no longer exists
 * fails too, so removing a pair is deleting its line. */

#include "test.h"
#include <rayforce.h>
#include "lang/eval.h"
#include "lang/env.h"
#include "core/runtime.h"
#include "table/sym.h"       /* ray_read_sym */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ray_runtime_t *__RUNTIME;

static void names_setup(void) { ray_runtime_create(0, NULL); }
static void names_teardown(void) { ray_runtime_destroy(__RUNTIME); }

/* Empty since #1826 removed stddev_pop, read-csv and write-csv: a pair added
 * here goes above the terminator, with its reason. */
static const struct { const char* a; const char* b; const char* reason; } k_allowed[] = {
    { NULL, NULL, NULL },
};
enum { N_ALLOWED = sizeof k_allowed / sizeof *k_allowed - 1 };

enum { MAX_FNS = 4096 };
typedef struct { char name[96]; int8_t type; int64_t fn; } fn_entry_t;
typedef struct { fn_entry_t e[MAX_FNS]; int n; } fn_set_t;

static bool is_builtin(const ray_t* v) {
    return v && (v->type == RAY_UNARY || v->type == RAY_BINARY || v->type == RAY_VARY);
}

/* Collect the builtins bound under `prefix`, walking namespace dicts. */
static void collect(fn_set_t* s, const char* prefix, ray_t* v, int depth) {
    if (!v || depth > 4 || s->n >= MAX_FNS) return;
    if (is_builtin(v)) {
        /* a namespace member may also be bound flat under its full name */
        for (int i = 0; i < s->n; i++) if (!strcmp(s->e[i].name, prefix)) return;
        fn_entry_t* e = &s->e[s->n++];
        snprintf(e->name, sizeof e->name, "%s", prefix);
        e->type = v->type;
        e->fn = v->i64;
        return;
    }
    if (v->type != RAY_DICT || prefix[0] != '.') return;   /* a namespace root: .csv, .sys, ... */
    ray_t* keys = ray_dict_keys(v);
    ray_t* vals = ray_dict_vals(v);
    if (!keys || !vals || keys->type != RAY_SYM || vals->type != RAY_LIST) return;
    for (int64_t i = 0; i < ray_len(keys); i++) {
        ray_t* k = ray_sym_str(ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs));
        if (!k) continue;
        char sub[96];
        snprintf(sub, sizeof sub, "%s.%.*s", prefix, (int)ray_str_len(k), ray_str_ptr(k));
        collect(s, sub, ((ray_t**)ray_data(vals))[i], depth + 1);
    }
}

static bool pair_allowed(const char* x, const char* y, bool* seen) {
    for (int i = 0; i < N_ALLOWED; i++)
        if ((!strcmp(x, k_allowed[i].a) && !strcmp(y, k_allowed[i].b)) ||
            (!strcmp(y, k_allowed[i].a) && !strcmp(x, k_allowed[i].b))) { seen[i] = true; return true; }
    return false;
}

static test_result_t test_no_two_names_one_function(void) {
    static fn_set_t s;
    s.n = 0;
    int32_t n = ray_env_global_count();
    int64_t* ids = (int64_t*)malloc((size_t)n * sizeof(int64_t));
    ray_t** vals = (ray_t**)malloc((size_t)n * sizeof(ray_t*));
    TEST_ASSERT_TRUE(ids && vals);
    n = ray_env_list(ids, vals, n);
    for (int32_t i = 0; i < n; i++) {
        ray_t* nm = ray_sym_str(ids[i]);
        if (!nm) continue;
        char name[96];
        snprintf(name, sizeof name, "%.*s", (int)ray_str_len(nm), ray_str_ptr(nm));
        collect(&s, name, vals[i], 0);
    }
    free(ids); free(vals);
    TEST_ASSERT_FMT(s.n > 300, "only %d builtins found", s.n);
    bool seen[N_ALLOWED + 1] = { false };
    for (int i = 0; i < s.n; i++)
        for (int j = i + 1; j < s.n; j++) {
            if (s.e[i].type != s.e[j].type || s.e[i].fn != s.e[j].fn) continue;
            TEST_ASSERT_FMT(pair_allowed(s.e[i].name, s.e[j].name, seen),
                            "%s and %s are one function: register one name, or list the pair with a reason",
                            s.e[i].name, s.e[j].name);
        }
    for (int i = 0; i < N_ALLOWED; i++)
        TEST_ASSERT_FMT(seen[i], "the listed pair %s/%s no longer shares a function: delete its line",
                        k_allowed[i].a, k_allowed[i].b);
    PASS();
}

const test_entry_t builtin_names_entries[] = {
    { "builtin_names/no_two_names_one_function", test_no_two_names_one_function, names_setup, names_teardown },
    { NULL, NULL, NULL, NULL },
};
