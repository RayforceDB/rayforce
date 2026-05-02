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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "core/types.h"
#include "lang/env.h"
#include "ops/ops.h"

/* Forward-declare runtime API (ray_fn_name test needs builtins registered). */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

static void types_runtime_setup(void) {
    ray_runtime_create(0, NULL);
}

static void types_runtime_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

/* ---- test_type_sizes_known_types --------------------------------------- */

static test_result_t test_type_sizes_known_types(void) {
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_BOOL], 1);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_U8], 1);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_I16], 2);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_I32], 4);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_I64], 8);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_F32], 4);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_F64], 8);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_DATE], 4);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_TIME], 4);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_TIMESTAMP], 8);
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_GUID], 16);

    PASS();
}

/* ---- test_elem_size_macro ---------------------------------------------- */

static test_result_t test_elem_size_macro(void) {
    /* ray_elem_size(t) should match ray_type_sizes[t] */
    TEST_ASSERT_EQ_U(ray_elem_size(RAY_I64), 8);
    TEST_ASSERT_EQ_U(ray_elem_size(RAY_I32), 4);
    TEST_ASSERT_EQ_U(ray_elem_size(RAY_BOOL), 1);
    TEST_ASSERT_EQ_U(ray_elem_size(RAY_GUID), 16);

    PASS();
}

/* ---- test_type_sizes_pointer_types ------------------------------------- */

static test_result_t test_type_sizes_pointer_types(void) {
    /* LIST is pointer-sized (8 bytes) */
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_LIST], 8);

    /* SYM default width is 8 (W64) */
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_SYM], 8);

    /* STR is 16 bytes (ray_str_t) */
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_STR], 16);

    /* SEL has no fixed element size */
    TEST_ASSERT_EQ_U(ray_type_sizes[RAY_SEL], 0);

    PASS();
}

/* ---- test_version_getters --------------------------------------------- */

static test_result_t test_version_getters(void) {
    /* Version components are non-negative integers. */
    int major = ray_version_major();
    int minor = ray_version_minor();
    int patch = ray_version_patch();

    TEST_ASSERT(major >= 0, "ray_version_major < 0");
    TEST_ASSERT(minor >= 0, "ray_version_minor < 0");
    TEST_ASSERT(patch >= 0, "ray_version_patch < 0");

    /* Must agree with the public macros (which the implementation echoes). */
    TEST_ASSERT_EQ_I(major, RAY_VERSION_MAJOR);
    TEST_ASSERT_EQ_I(minor, RAY_VERSION_MINOR);
    TEST_ASSERT_EQ_I(patch, RAY_VERSION_PATCH);

    PASS();
}

/* ---- test_version_string ----------------------------------------------- */

static test_result_t test_version_string(void) {
    const char* s = ray_version_string();
    TEST_ASSERT_NOT_NULL(s);

    /* Shape: "MAJOR.MINOR.PATCH" — at minimum two dots, all-digit between. */
    int dots = 0;
    int digits = 0;
    for (const char* p = s; *p; p++) {
        if (*p == '.') dots++;
        else if (*p >= '0' && *p <= '9') digits++;
        else FAILF("unexpected character '%c' in version string \"%s\"", *p, s);
    }
    TEST_ASSERT_EQ_I(dots, 2);
    TEST_ASSERT(digits >= 3, "version string has fewer than 3 digits");

    /* Parse back and compare with the integer getters. */
    int maj = 0, min = 0, pat = 0;
    int n = sscanf(s, "%d.%d.%d", &maj, &min, &pat);
    TEST_ASSERT_EQ_I(n, 3);
    TEST_ASSERT_EQ_I(maj, ray_version_major());
    TEST_ASSERT_EQ_I(min, ray_version_minor());
    TEST_ASSERT_EQ_I(pat, ray_version_patch());

    PASS();
}

/* ---- test_fn_name_builtin ---------------------------------------------- */
/* ray_fn_name reads the function-object name from nullmap[2..15].  After
 * ray_runtime_create the global env contains builtins like "+", "sum", and
 * "println"; looking them up and reading the name back round-trips the
 * fn_set_name encoding inside env.c. */

static test_result_t test_fn_name_builtin(void) {
    /* "+" — single byte name, fits in nullmap[2..15] easily. */
    int64_t plus_id = ray_sym_intern("+", 1);
    ray_t* plus = ray_env_get(plus_id);
    TEST_ASSERT_NOT_NULL(plus);
    /* Builtin "+" is registered as a binary fn. */
    TEST_ASSERT_EQ_I(plus->type, RAY_BINARY);

    const char* plus_name = ray_fn_name(plus);
    TEST_ASSERT_NOT_NULL(plus_name);
    TEST_ASSERT_STR_EQ(plus_name, "+");

    /* "sum" — multi-character name to exercise the memcpy path. */
    int64_t sum_id = ray_sym_intern("sum", 3);
    ray_t* sum = ray_env_get(sum_id);
    TEST_ASSERT_NOT_NULL(sum);

    const char* sum_name = ray_fn_name(sum);
    TEST_ASSERT_NOT_NULL(sum_name);
    TEST_ASSERT_STR_EQ(sum_name, "sum");

    /* The pointer must be inside the function object's nullmap region
     * (offset 2 from the nullmap base). */
    TEST_ASSERT_EQ_PTR(sum_name, (const char*)sum->nullmap + 2);

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t types_entries[] = {
    { "types/sizes_known_types", test_type_sizes_known_types, NULL, NULL },
    { "types/elem_size_macro", test_elem_size_macro, NULL, NULL },
    { "types/sizes_pointer_types", test_type_sizes_pointer_types, NULL, NULL },
    { "types/version_getters", test_version_getters, NULL, NULL },
    { "types/version_string", test_version_string, NULL, NULL },
    { "types/fn_name_builtin", test_fn_name_builtin, types_runtime_setup, types_runtime_teardown },
    { NULL, NULL, NULL, NULL },
};


