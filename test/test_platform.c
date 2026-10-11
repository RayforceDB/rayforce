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

/* pwrite, ftruncate: POSIX beyond C17. */
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
/* /proc/meminfo's `key`, kB, or -1. */
static int64_t plat_meminfo(const char* key) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    int64_t v = -1;
    size_t k = strlen(key);
    while (v < 0 && fgets(line, sizeof(line), f))
        if (strncmp(line, key, k) == 0 && line[k] == ':') v = strtoll(line + k + 1, NULL, 10);
    fclose(f);
    return v;
}
#endif

/* The cgroup directory goes with the limit: there is one exactly when a
 * limit is in force, and the RAM in use reads from it.  The RAM in use is all
 * but free memory, clean page cache and reclaimable slab, dirty pages
 * included: from memory.current and memory.stat under cgroup v2, from
 * memory.usage_in_bytes and the hierarchical total_* counters of memory.stat
 * under v1 (here in a directory of the test's own), and from /proc/meminfo
 * for the machine. */
static test_result_t test_cgroup_mem_used(void) {
    char dir[512];
    bool has = ray_os_cgroup_mem_dir(dir, sizeof(dir));
    TEST_ASSERT(has == (ray_os_cgroup_mem_limit() > 0), "a cgroup directory exactly with a limit");
    int64_t used = -1;
    if (has) TEST_ASSERT(ray_os_mem_used(dir, &used) && used > 0, "and it reads");
#ifdef RAY_OS_LINUX
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "/tmp/rayforce_cg_%d", (int)getpid());
    TEST_ASSERT(mkdir(tmp, 0700) == 0, "a cgroup directory of our own");
    plat_write(tmp, "memory.current", "1000000000\n");
    plat_write(tmp, "memory.stat", "anon 600000000\nfile 300000000\nkernel 100000000\n"
               "shmem 50000000\nfile_mapped 0\nfile_dirty 40000000\nfile_writeback 10000000\n"
               "inactive_anon 1\nactive_anon 1\ninactive_file 150000000\nactive_file 100000000\n"
               "slab_reclaimable 30000000\n");
    int64_t u2 = -1;
    bool v2 = ray_os_mem_used(tmp, &u2);
    plat_write(tmp, "memory.stat", "anon 600000000\nfile 300000000\nshmem 50000000\n"
               "file_writeback 10000000\ninactive_file 150000000\nactive_file 100000000\n");
    bool partial = ray_os_mem_used(tmp, &used);   /* no file_dirty */
    plat_unlink(tmp, "memory.current");
    plat_write(tmp, "memory.usage_in_bytes", "2000000000\n");
    plat_write(tmp, "memory.stat", "cache 1\nrss 1\nshmem 1\ndirty 1\nwriteback 1\n"
               "inactive_file 1\nactive_file 1\n"
               "total_cache 500000000\ntotal_rss 1300000000\ntotal_shmem 100000000\n"
               "total_dirty 60000000\ntotal_writeback 20000000\n"
               "total_inactive_file 250000000\ntotal_active_file 150000000\n");
    int64_t u1 = -1;
    bool v1 = ray_os_mem_used(tmp, &u1);
    plat_unlink(tmp, "memory.usage_in_bytes");
    plat_unlink(tmp, "memory.stat");
    rmdir(tmp);
    TEST_ASSERT(v2 && u2 == 770000000,
                "v2: current less the clean file cache and reclaimable slab");
    TEST_ASSERT(!partial, "a field missing: no reading");
    TEST_ASSERT(v1 && u1 == 1680000000, "v1: usage less the clean total cache");

    int64_t total = plat_meminfo("MemTotal"), freek = plat_meminfo("MemFree");
    int64_t buffers = plat_meminfo("Buffers"), cached = plat_meminfo("Cached");
    int64_t shmem = plat_meminfo("Shmem"), slab = plat_meminfo("SReclaimable");
    int64_t d = plat_meminfo("Dirty"), wb = plat_meminfo("Writeback");
    bool host = ray_os_mem_used(NULL, &used);
    int64_t want = (total - freek - slab - (buffers + cached - shmem - d - wb)) << 10;
    int64_t off = used - want;
    const int64_t slack = INT64_C(256) << 20;
    TEST_ASSERT(host && total > 0, "the machine's use reads");
    TEST_ASSERT(off > -slack && off < slack,
                "the machine: all but free memory, clean page cache and reclaimable slab");
#else
    TEST_ASSERT(!has && !ray_os_mem_used(NULL, &used), "no cgroups, no use read");
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
    { "platform/cgroup_mem_used", test_cgroup_mem_used, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


