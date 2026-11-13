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
#include <sys/wait.h>
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

#ifdef RAY_OS_LINUX
#include <dirent.h>
/* Descriptors open in this process (the directory's own among them). */
static int plat_nfds(void) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d)) n++;
    closedir(d);
    return n;
}
#endif

/* Write-behind holds at most RAY_FILE_WB_FDS descriptors: past them a file
 * is not begun (-1) and a start is done by the caller.  The background
 * thread closes every one it was handed, and the files keep what was
 * written. */
static test_result_t test_file_writebehind_fds(void) {
#ifdef RAY_OS_LINUX
    enum { N = RAY_FILE_WB_FDS + 44 };
    char dir[64], path[128];
    snprintf(dir, sizeof(dir), "/tmp/rayforce_wb_%d", (int)getpid());
    TEST_ASSERT(mkdir(dir, 0700) == 0, "a directory of our own");
    int base = plat_nfds(), peak = 0, begun = 0;
    static int fds[N];
    for (int i = 0; i < N; i++) {
        char name[16];
        snprintf(name, sizeof(name), "f%d", i);
        plat_write(dir, name, "written once");
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        fds[i] = ray_file_writebehind_begin(path);
        begun += fds[i] >= 0;
        int n = plat_nfds() - base;
        if (n > peak) peak = n;
    }
    for (int i = 0; i < N; i++) ray_file_writebehind_end(fds[i]);
    snprintf(path, sizeof(path), "%s/f0", dir);
    int wfd = open(path, O_WRONLY);
    for (int k = 0; k < 4 * RAY_FILE_WB_FDS; k++) {
        ray_file_writeback_start(wfd, 0, 12);
        int n = plat_nfds() - base;
        if (n > peak) peak = n;
    }
    close(wfd);
    (void)ray_file_writeback_errors();   /* every request served */
    int left = plat_nfds() - base;
    /* the bound is free again: a whole round more is begun */
    int again = 0;
    for (int i = 0; i < RAY_FILE_WB_FDS / 2; i++) {
        snprintf(path, sizeof(path), "%s/f%d", dir, i);
        fds[i] = ray_file_writebehind_begin(path);
        again += fds[i] >= 0;
    }
    for (int i = 0; i < RAY_FILE_WB_FDS / 2; i++) ray_file_writebehind_end(fds[i]);
    int same = 0;
    for (int i = 0; i < N; i++) {
        char name[16], buf[32] = {0};
        snprintf(name, sizeof(name), "f%d", i);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        int fd = open(path, O_RDONLY);
        if (fd >= 0 && read(fd, buf, sizeof(buf) - 1) == 12 && !strcmp(buf, "written once")) same++;
        if (fd >= 0) close(fd);
        plat_unlink(dir, name);
    }
    rmdir(dir);
    TEST_ASSERT(begun > 0 && begun < N, "write-behind begun up to the bound");
    TEST_ASSERT(peak <= RAY_FILE_WB_FDS + 1, "no more descriptors held than the bound");
    TEST_ASSERT(left <= 0, "every descriptor handed over closed");
    TEST_ASSERT_EQ_I(again, RAY_FILE_WB_FDS / 2);
    TEST_ASSERT_EQ_I(same, N);
#endif
    PASS();
}

#ifdef RAY_OS_LINUX
/* Threads of this process running the write-behind loop (named "ray-wb"). */
static int plat_wb_threads(void) {
    DIR* d = opendir("/proc/self/task");
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[sizeof("/proc/self/task/") + sizeof(e->d_name) + sizeof("/comm")], comm[32] = {0};
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;   /* a task that ended meanwhile */
        if (fgets(comm, sizeof(comm), f) && !strcmp(comm, "ray-wb\n")) n++;
        fclose(f);
    }
    closedir(d);
    return n;
}

/* No write-behind thread left: one joined drops its task a moment after it
 * wakes the joiner. */
static int plat_wb_threads_left(void) {
    int n = plat_wb_threads();
    for (int i = 0; i < 100000 && n > 0; i++) { sched_yield(); n = plat_wb_threads(); }
    return n;
}
#endif

/* A failed writeback is counted, once every request is served, and the
 * background thread has ended by then: a pipe (no writeback) and, with
 * the hook, a write-behind of a file. */
static test_result_t test_file_writeback_errors(void) {
#ifdef RAY_OS_LINUX
    int64_t e0 = ray_file_writeback_errors();
    int p[2];
    TEST_ASSERT(pipe(p) == 0, "a pipe");
    ray_file_writeback_start(p[1], 0, 1);
    int64_t e1 = ray_file_writeback_errors();
    int left = plat_wb_threads_left();
    close(p[0]); close(p[1]);
    TEST_ASSERT_EQ_I(e1 - e0, 1);
    TEST_ASSERT_EQ_I(left, 0);
#ifdef DEBUG
    char dir[64], path[128];
    snprintf(dir, sizeof(dir), "/tmp/rayforce_wbe_%d", (int)getpid());
    TEST_ASSERT(mkdir(dir, 0700) == 0, "a directory of our own");
    plat_write(dir, "f", "written once");
    snprintf(path, sizeof(path), "%s/f", dir);
    ray_file_writeback_test_fail(true);
    ray_file_writeback_test_hold(true);
    int fd = ray_file_writebehind_begin(path);
    ray_file_writebehind_end(fd);
    ray_file_writeback_test_wait_held();
    int serving = plat_wb_threads();   /* the thread, found by its name */
    ray_file_writeback_test_hold(false);
    int64_t e2 = ray_file_writeback_errors();
    ray_file_writeback_test_fail(false);
    left = plat_wb_threads_left();
    plat_unlink(dir, "f");
    rmdir(dir);
    TEST_ASSERT(fd >= 0, "write-behind begun");
    TEST_ASSERT_EQ_I(serving, 1);
    TEST_ASSERT_EQ_I(e2 - e1, 2);
    TEST_ASSERT_EQ_I(left, 0);
#endif
#endif
    PASS();
}

/* A fork while the background thread serves a request: the child closes
 * that request's descriptor as well as the queued ones (the parent's to
 * serve), and the parent's are all closed once they are served. */
static test_result_t test_file_writeback_fork(void) {
#if defined(RAY_OS_LINUX) && defined(DEBUG)
    char dir[64], path[128];
    snprintf(dir, sizeof(dir), "/tmp/rayforce_wbk_%d", (int)getpid());
    TEST_ASSERT(mkdir(dir, 0700) == 0, "a directory of our own");
    plat_write(dir, "f", "written once");
    snprintf(path, sizeof(path), "%s/f", dir);
    (void)ray_file_writeback_errors();
    int base = plat_nfds();
    ray_file_writeback_test_hold(true);
    int fd = ray_file_writebehind_begin(path);   /* its start served, held there */
    if (fd >= 0) ray_file_writeback_test_wait_held();
    ray_file_writebehind_end(fd);                /* queued behind it */
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) _exit(plat_nfds() == base ? 0 : 1);
    int status = 0;
    pid_t got = pid > 0 ? waitpid(pid, &status, 0) : -1;
    ray_file_writeback_test_hold(false);
    (void)ray_file_writeback_errors();
    int left = plat_nfds() - base;
    plat_unlink(dir, "f");
    rmdir(dir);
    TEST_ASSERT(fd >= 0, "write-behind begun");
    TEST_ASSERT(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "the child holds no descriptor of the parent's requests");
    TEST_ASSERT_EQ_I(left, 0);
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
    { "platform/file_writebehind_fds", test_file_writebehind_fds, NULL, NULL },
    { "platform/file_writeback_errors", test_file_writeback_errors, NULL, NULL },
    { "platform/file_writeback_fork", test_file_writeback_fork, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


