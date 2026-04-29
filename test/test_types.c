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
#include "ops/ops.h"

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

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t types_entries[] = {
    { "types/sizes_known_types", test_type_sizes_known_types, NULL, NULL },
    { "types/elem_size_macro", test_elem_size_macro, NULL, NULL },
    { "types/sizes_pointer_types", test_type_sizes_pointer_types, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


