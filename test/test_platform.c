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
#ifdef RAY_OS_LINUX
#include <sched.h>
#endif

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

#ifdef RAY_OS_LINUX
/* Writes `text` to root/rel, creating its directories. */
static void cgq_put(const char* root, const char* rel, const char* text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    for (char* p = path + strlen(root) + 1; *p; p++)
        if (*p == '/') { *p = 0; (void)mkdir(path, 0700); *p = '/'; }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t n = write(fd, text, strlen(text));
    (void)n;
    close(fd);
}

/* The quota of a fake /proc + /sys under a fresh directory. */
static int64_t cgq_quota(const char* tag, const char* cgroup, const char* mountinfo,
                         const char* const* files) {
    char root[128], cmd[300];
    snprintf(root, sizeof(root), "/tmp/rayforce_cgq_%d_%s", (int)getpid(), tag);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    if (system(cmd) != 0) return -2;
    (void)mkdir(root, 0700);
    cgq_put(root, "proc/self/cgroup", cgroup);
    if (mountinfo) cgq_put(root, "proc/self/mountinfo", mountinfo);
    for (int i = 0; files[i]; i += 2) cgq_put(root, files[i], files[i + 1]);
    ray_os_cgroup_test_root(root);
    int64_t q = ray_os_cgroup_cpu_quota();
    ray_os_cgroup_test_root(NULL);
    if (system(cmd) != 0) return -3;
    return q;
}
#endif

/* The CPU quota is found through the mount the hierarchy is visible at:
 * its mount point and the subtree it shows (/proc/self/mountinfo), for v2
 * and for a v1 cpu controller wherever mounted; the smallest over the
 * cgroup and its visible ancestors; the conventional mounts without
 * mountinfo. */
static test_result_t test_cgroup_cpu_quota(void) {
    int64_t real = ray_os_cgroup_cpu_quota();
    TEST_ASSERT(real >= 0, "a quota is never negative");
#ifdef RAY_OS_LINUX
    const char* v2mi = "30 25 0:26 / /sys/fs/cgroup rw,nosuid shared:9 - cgroup2 cgroup2 rw\n";
    const char* const canon[] = { "sys/fs/cgroup/a/b/cpu.max", "150000 100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("v2", "0::/a/b\n", v2mi, canon), 1500);
    /* the v2 subtree /tenant mounted at /sys/fs/cgroup: /tenant/job lives at
     * /sys/fs/cgroup/job, and the unlimited visible root does not count */
    const char* const sub[] = { "sys/fs/cgroup/job/cpu.max", "150000 100000\n",
                                "sys/fs/cgroup/cpu.max", "max 100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("sub", "0::/tenant/job\n",
                               "30 25 0:26 /tenant /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n", sub), 1500);
    /* a membership outside the visible subtree has no readable quota */
    TEST_ASSERT_EQ_I(cgq_quota("out", "0::/other/job\n",
                               "30 25 0:26 /tenant /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n", sub), 0);
    /* several views of the hierarchy: the one holding the membership counts,
     * whatever comes first in mountinfo */
    const char* const lim[] = { "sys/fs/cgroup/limited/cpu.max", "150000 100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("multi", "0::/limited\n",
                               "30 25 0:26 /other /unrelated rw - cgroup2 cgroup2 rw\n"
                               "31 25 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n", lim), 1500);
    /* a narrow view and a full one: the ancestor quota only the full view
     * shows is the tighter one */
    const char* const both[] = { "narrow/leaf/cpu.max", "150000 100000\n",
                                 "sys/fs/cgroup/org/tenant/leaf/cpu.max", "150000 100000\n",
                                 "sys/fs/cgroup/org/cpu.max", "50000 100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("views", "0::/org/tenant/leaf\n",
                               "30 25 0:26 /org/tenant /narrow rw - cgroup2 cgroup2 rw\n"
                               "31 25 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n", both), 500);
    /* a v1 cpu controller mounted off the conventional place */
    const char* const v1[] = { "cgroup/cpu/x/cpu.cfs_quota_us", "50000\n",
                               "cgroup/cpu/x/cpu.cfs_period_us", "100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("v1", "4:cpu,cpuacct:/x\n",
                               "40 25 0:30 / /cgroup/cpu rw - cgroup cgroup rw,cpu,cpuacct\n", v1), 500);
    /* a tighter ancestor wins (a quarter of a CPU, in millicpus) */
    const char* const anc[] = { "sys/fs/cgroup/a/b/cpu.max", "300000 100000\n",
                                "sys/fs/cgroup/a/cpu.max", "25000 100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("anc", "0::/a/b\n", v2mi, anc), 250);
    const char* const none[] = { "sys/fs/cgroup/a/b/cpu.max", "max 100000\n", NULL };
    TEST_ASSERT_EQ_I(cgq_quota("none", "0::/a/b\n", v2mi, none), 0);
    /* no mountinfo: the conventional /sys/fs/cgroup */
    TEST_ASSERT_EQ_I(cgq_quota("nomi", "0::/a/b\n", NULL, canon), 1500);
    TEST_ASSERT_EQ_I(ray_os_cgroup_cpu_quota(), real);
#else
    TEST_ASSERT_EQ_I(real, 0);
#endif
    PASS();
}

/* A kernel whose CPU mask is wider than a cpu_set_t rejects the fixed
 * buffer (EINVAL); the count retries with a larger one instead of falling
 * back to every online CPU. */
static test_result_t test_cpu_set_count_wide_mask(void) {
    uint32_t n = ray_cpu_set_count();
    TEST_ASSERT(n >= 1, "at least one CPU");
#ifdef RAY_OS_LINUX
    cpu_set_t set, two;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0)
        SKIP("the kernel's CPU mask is wider than a cpu_set_t: no fixed-size snapshot");
    TEST_ASSERT_EQ_I(n, CPU_COUNT(&set));
    if (CPU_COUNT(&set) < 3) SKIP("needs at least 3 CPUs");
    /* two CPUs of the mask, so the fallback (every online CPU) differs */
    CPU_ZERO(&two);
    for (int c = 0, k = 0; c < CPU_SETSIZE && k < 2; c++)
        if (CPU_ISSET(c, &set)) { CPU_SET(c, &two); k++; }
    int rc = sched_setaffinity(0, sizeof(two), &two);
    ray_cpu_set_test_min_bytes(4 * sizeof(cpu_set_t));
    uint32_t wide = ray_cpu_set_count();
    ray_cpu_set_test_min_bytes(0);
    (void)sched_setaffinity(0, sizeof(set), &set);
    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_EQ_I(wide, 2);
#endif
    PASS();
}

const test_entry_t platform_entries[] = {
    { "platform/vm_alloc_free", test_vm_alloc_free, NULL, NULL },
    { "platform/vm_alloc_aligned", test_vm_alloc_aligned, NULL, NULL },
    { "platform/thread_count", test_thread_count, NULL, NULL },
    { "platform/thread_create_join", test_thread_create_join, NULL, NULL },
    { "platform/cgroup_mem_limit", test_cgroup_mem_limit, NULL, NULL },
    { "platform/cgroup_stat_flush", test_cgroup_stat_flush, NULL, NULL },
    { "platform/cgroup_cpu_quota", test_cgroup_cpu_quota, NULL, NULL },
    { "platform/cpu_set_count_wide_mask", test_cpu_set_count_wide_mask, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


