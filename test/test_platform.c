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

/* open, mkdir, getpid: POSIX beyond C17. */
#if !defined(_GNU_SOURCE) && !defined(_WIN32)
#define _GNU_SOURCE
#endif
#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "core/platform.h"
#include "mem/heap.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

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

#ifdef RAY_OS_LINUX
static void plat_write(const char* dir, const char* name, const char* text) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t n = write(fd, text, strlen(text));
    (void)n;
    close(fd);
}
static void plat_unlink(const char* dir, const char* name) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unlink(path);
}
#endif

/* The cgroup directory goes with the limit: there is one exactly when a
 * limit is in force.  Its memory.stat is read for what reading does — the
 * kernel flushes the cgroup's statistics — here in a directory of the
 * test's own too, and a directory without the file reads nothing. */
static test_result_t test_cgroup_stat_flush(void) {
    char dir[512];
    bool has = ray_os_cgroup_mem_dir(dir, sizeof(dir));
    TEST_ASSERT(has == (ray_os_cgroup_mem_limit() > 0), "a cgroup directory exactly with a limit");
    if (has) TEST_ASSERT(ray_os_cgroup_stat_flush(dir), "and its statistics read");
    TEST_ASSERT(!ray_os_cgroup_stat_flush(NULL) && !ray_os_cgroup_stat_flush(""),
                "no cgroup: nothing read");
#ifdef RAY_OS_LINUX
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/rayforce_cg_%d", (int)getpid());
    TEST_ASSERT(mkdir(tmp, 0700) == 0, "a cgroup directory of our own");
    bool none = !ray_os_cgroup_stat_flush(tmp);
    plat_write(tmp, "memory.stat", "anon 1\nfile 2\n");
    bool read = ray_os_cgroup_stat_flush(tmp);
    plat_unlink(tmp, "memory.stat");
    rmdir(tmp);
    TEST_ASSERT(none, "no memory.stat: nothing read");
    TEST_ASSERT(read, "its memory.stat read");
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
    { "platform/cgroup_stat_flush", test_cgroup_stat_flush, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


