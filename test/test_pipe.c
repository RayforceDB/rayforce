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
#include "ops/pipe.h"

/* ---- test_pipe_new_defaults -------------------------------------------- */

static test_result_t test_pipe_new_defaults(void) {
    ray_pipe_t* p = ray_pipe_new();
    TEST_ASSERT_NOT_NULL(p);

    /* All fields should be zero-initialized */
    TEST_ASSERT_NULL(p->op);
    TEST_ASSERT_NULL(p->inputs[0]);
    TEST_ASSERT_NULL(p->inputs[1]);
    TEST_ASSERT_NULL(p->materialized);

    /* spill_fd should be -1 (no spill file) */
    TEST_ASSERT_EQ_I(p->spill_fd, -1);

    ray_pipe_free(p);
    PASS();
}

/* ---- test_pipe_free_null_safe ------------------------------------------ */

static test_result_t test_pipe_free_null_safe(void) {
    /* Freeing NULL should not crash */
    ray_pipe_free(NULL);

    PASS();
}

/* ---- test_pipe_multiple_alloc_free ------------------------------------- */

static test_result_t test_pipe_multiple_alloc_free(void) {
    /* Allocate several pipes, verify independence, free them */
    ray_pipe_t* p1 = ray_pipe_new();
    ray_pipe_t* p2 = ray_pipe_new();
    ray_pipe_t* p3 = ray_pipe_new();

    TEST_ASSERT_NOT_NULL(p1);
    TEST_ASSERT_NOT_NULL(p2);
    TEST_ASSERT_NOT_NULL(p3);

    /* They should be distinct allocations */
    TEST_ASSERT_TRUE(p1 != p2);
    TEST_ASSERT_TRUE(p2 != p3);
    TEST_ASSERT_TRUE(p1 != p3);

    /* Wire p1 as input to p2 */
    p2->inputs[0] = p1;
    TEST_ASSERT_TRUE(p2->inputs[0] == p1);

    /* Free in reverse; ray_pipe_free does NOT recurse into inputs */
    ray_pipe_free(p3);
    ray_pipe_free(p2);
    ray_pipe_free(p1);

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t pipe_entries[] = {
    { "pipe/new_defaults", test_pipe_new_defaults, NULL, NULL },
    { "pipe/free_null_safe", test_pipe_free_null_safe, NULL, NULL },
    { "pipe/multiple_alloc_free", test_pipe_multiple_alloc_free, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


