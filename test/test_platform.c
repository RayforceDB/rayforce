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
#include "core/platform.h"
#include "mem/heap.h"
#include <stdatomic.h>
#include <string.h>

/* ---- test_vm_alloc_free ------------------------------------------------ */

static test_result_t test_vm_alloc_free(void) {
    size_t size = 4096;
    void* p = ray_vm_alloc(size);
    TEST_ASSERT_NOT_NULL(p);

    /* Should be writable */
    memset(p, 0xAB, size);

    ray_vm_free(p, size);
    PASS();
}

/* ---- test_vm_alloc_aligned --------------------------------------------- */

static test_result_t test_vm_alloc_aligned(void) {
    size_t alignment = 64 * 1024;  /* 64 KB alignment */
    size_t size = 4096;
    void* p = ray_vm_alloc_aligned(size, alignment);
    TEST_ASSERT_NOT_NULL(p);

    /* Verify alignment */
    TEST_ASSERT_EQ_U((uintptr_t)p % alignment, 0);

    /* Should be writable */
    memset(p, 0xCD, size);

    ray_vm_free(p, size);
    PASS();
}

/* ---- test_thread_count ------------------------------------------------- */

static test_result_t test_thread_count(void) {
    uint32_t count = ray_thread_count();
    TEST_ASSERT((count) >= (1), "count >= 1");

    PASS();
}

/* ---- test_thread_create_join ------------------------------------------- */

static _Atomic(int) g_thread_ran = 0;

static void thread_fn(void* arg) {
    (void)arg;
    atomic_store(&g_thread_ran, 1);
}

static test_result_t test_thread_create_join(void) {
    atomic_store(&g_thread_ran, 0);

    ray_thread_t t;
    ray_err_t err = ray_thread_create(&t, thread_fn, NULL);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    err = ray_thread_join(t);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    TEST_ASSERT_EQ_I(atomic_load(&g_thread_ran), 1);

    PASS();
}

/* ---- test_cgroup_mem_limit --------------------------------------------- */

/* The probe must be defensive: never negative, and never larger than the
 * host's physical RAM when a container limit is in force.  Whether a limit
 * IS in force depends on how the suite is launched (docker --memory,
 * systemd MemoryMax), so only the invariants are asserted here; the
 * limited case is verified by hand with `systemd-run --scope -p MemoryMax`
 * (issue #688). */
static test_result_t test_cgroup_mem_limit(void) {
    int64_t cg = ray_os_cgroup_mem_limit();
    TEST_ASSERT(cg >= 0, "cgroup limit is never negative");
    /* A second probe reads the same files and must agree. */
    TEST_ASSERT_EQ_I(ray_os_cgroup_mem_limit(), cg);
#ifndef RAY_OS_LINUX
    TEST_ASSERT_EQ_I(cg, 0);
#endif
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t platform_entries[] = {
    { "platform/vm_alloc_free", test_vm_alloc_free, NULL, NULL },
    { "platform/vm_alloc_aligned", test_vm_alloc_aligned, NULL, NULL },
    { "platform/thread_count", test_thread_count, NULL, NULL },
    { "platform/thread_create_join", test_thread_create_join, NULL, NULL },
    { "platform/cgroup_mem_limit", test_cgroup_mem_limit, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


