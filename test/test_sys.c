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

#define _POSIX_C_SOURCE 200809L  /* setenv / unsetenv / strdup: env_restore.h */
#include "test.h"
#include <rayforce.h>
#include "mem/sys.h"
#include "env_restore.h"
#include "core/platform.h"   /* ray_env_flag */
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

/* The runner puts the environment back after every test (test/main.c,
 * run_one): a variable a test added goes, one it changed or removed comes
 * back as it was.  A test that set RAY_CSV_TRACE and stopped at a failure
 * before clearing it left every later test, and every process those
 * started, tracing (#1552). */
static test_result_t test_sys_env_restore(void) {
#if defined(_WIN32)
    SKIP("the runner restores the environment on POSIX only");
#else
    setenv("RFL_ENV_RESTORE_KEEP", "kept", 1);
    setenv("RFL_ENV_RESTORE_GONE", "here", 1);
    unsetenv("RFL_ENV_RESTORE_ADDED");
    char** snap = env_snapshot();
    TEST_ASSERT_NOT_NULL(snap);
    setenv("RFL_ENV_RESTORE_ADDED", "1", 1);      /* added by the "test" */
    setenv("RFL_ENV_RESTORE_KEEP", "changed", 1); /* changed */
    unsetenv("RFL_ENV_RESTORE_GONE");             /* removed */
    env_restore(snap);
    bool added_gone = getenv("RFL_ENV_RESTORE_ADDED") == NULL;
    const char* keep = getenv("RFL_ENV_RESTORE_KEEP");
    const char* gone = getenv("RFL_ENV_RESTORE_GONE");
    bool kept = keep && strcmp(keep, "kept") == 0;
    bool back = gone && strcmp(gone, "here") == 0;
    unsetenv("RFL_ENV_RESTORE_KEEP");
    unsetenv("RFL_ENV_RESTORE_GONE");
    TEST_ASSERT(added_gone, "a variable the test added is removed");
    TEST_ASSERT(kept, "a variable the test changed is set back");
    TEST_ASSERT(back, "a variable the test removed is set again");
    PASS();
#endif
}

/* An on/off switch is on only with a value other than "" and "0":
 * RAY_CSV_TRACE= (empty, as (.os.setenv "RAY_CSV_TRACE" "") leaves it, the
 * language having no unsetenv) is off, for this process and the ones it
 * starts (#1552), and so is RAY_CSV_TRACE=0, as for RAY_DFD and
 * RAYFORCE_NO_FSYNC. */
static test_result_t test_sys_env_flag_empty_off(void) {
#if defined(_WIN32)
    SKIP("setenv is POSIX");
#else
    unsetenv("RFL_ENV_FLAG_PROBE");
    bool unset_off = !ray_env_flag("RFL_ENV_FLAG_PROBE");
    setenv("RFL_ENV_FLAG_PROBE", "", 1);
    bool empty_off = !ray_env_flag("RFL_ENV_FLAG_PROBE");
    setenv("RFL_ENV_FLAG_PROBE", "1", 1);
    bool set_on = ray_env_flag("RFL_ENV_FLAG_PROBE");
    setenv("RFL_ENV_FLAG_PROBE", "0", 1);
    bool zero_off = !ray_env_flag("RFL_ENV_FLAG_PROBE");
    setenv("RFL_ENV_FLAG_PROBE", "yes", 1);
    bool word_on = ray_env_flag("RFL_ENV_FLAG_PROBE");
    unsetenv("RFL_ENV_FLAG_PROBE");
    TEST_ASSERT(unset_off, "unset is off");
    TEST_ASSERT(empty_off, "an empty value is off");
    TEST_ASSERT(set_on, "1 is on");
    TEST_ASSERT(zero_off, "0 is off");
    TEST_ASSERT(word_on, "any other value is on");
    PASS();
#endif
}

const test_entry_t sys_entries[] = {
    { "sys/alloc_free", test_sys_alloc_free, NULL, NULL },
    { "sys/env_restore", test_sys_env_restore, NULL, NULL },
    { "sys/env_flag_empty_off", test_sys_env_flag_empty_off, NULL, NULL },
    { "sys/realloc", test_sys_realloc, NULL, NULL },
    { "sys/strdup", test_sys_strdup, NULL, NULL },
    { "sys/get_stat", test_sys_get_stat, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


