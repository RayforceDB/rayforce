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
#include "mem/sys.h"
#include <string.h>

/* ---- test_sys_alloc_free ----------------------------------------------- */

static test_result_t test_sys_alloc_free(void) {
    void* p = ray_sys_alloc(128);
    TEST_ASSERT_NOT_NULL(p);

    /* Should be writable */
    memset(p, 0x42, 128);

    ray_sys_free(p);

    /* Free NULL should be safe */
    ray_sys_free(NULL);

    PASS();
}

/* ---- test_sys_realloc -------------------------------------------------- */

static test_result_t test_sys_realloc(void) {
    /* realloc(NULL, n) should behave like alloc */
    void* p = ray_sys_realloc(NULL, 64);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xAA, 64);

    /* Grow the allocation */
    void* p2 = ray_sys_realloc(p, 8192);
    TEST_ASSERT_NOT_NULL(p2);

    /* First 64 bytes should be preserved */
    uint8_t* bytes = (uint8_t*)p2;
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQ_U(bytes[i], 0xAA);
    }

    ray_sys_free(p2);

    /* realloc(ptr, 0) should free and return NULL */
    void* p3 = ray_sys_alloc(32);
    TEST_ASSERT_NOT_NULL(p3);
    void* p4 = ray_sys_realloc(p3, 0);
    TEST_ASSERT_NULL(p4);

    PASS();
}

/* ---- test_sys_strdup --------------------------------------------------- */

static test_result_t test_sys_strdup(void) {
    char* dup = ray_sys_strdup("hello");
    TEST_ASSERT_NOT_NULL(dup);
    TEST_ASSERT_STR_EQ(dup, "hello");
    ray_sys_free(dup);

    /* NULL input should return NULL */
    TEST_ASSERT_NULL(ray_sys_strdup(NULL));

    /* Empty string */
    char* empty = ray_sys_strdup("");
    TEST_ASSERT_NOT_NULL(empty);
    TEST_ASSERT_STR_EQ(empty, "");
    ray_sys_free(empty);

    PASS();
}

/* ---- test_sys_get_stat ------------------------------------------------- */

static test_result_t test_sys_get_stat(void) {
    int64_t current_before, peak_before;
    ray_sys_get_stat(&current_before, &peak_before);

    void* p = ray_sys_alloc(4096);
    TEST_ASSERT_NOT_NULL(p);

    int64_t current_during, peak_during;
    ray_sys_get_stat(&current_during, &peak_during);
    TEST_ASSERT((current_during) > (current_before), "current_during > current_before");
    TEST_ASSERT((peak_during) >= (current_during), "peak_during >= current_during");

    ray_sys_free(p);

    int64_t current_after, peak_after;
    ray_sys_get_stat(&current_after, &peak_after);
    TEST_ASSERT((current_after) < (current_during), "current_after < current_during");
    /* Peak should not decrease */
    TEST_ASSERT((peak_after) >= (peak_during), "peak_after >= peak_during");

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t sys_entries[] = {
    { "sys/alloc_free", test_sys_alloc_free, NULL, NULL },
    { "sys/realloc", test_sys_realloc, NULL, NULL },
    { "sys/strdup", test_sys_strdup, NULL, NULL },
    { "sys/get_stat", test_sys_get_stat, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


