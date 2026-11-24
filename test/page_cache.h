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

/* What tests that measure the page cache can know about it.  A test that
 * asks pages ahead and then counts its major faults assumes the pages stay
 * cached until they are read; reclaim can drop them first, and the read
 * back is a fault the code under test did not cause.  The kernel counts
 * such reads back of reclaimed file pages as refaults: a fault budget is
 * raised by the refaults counted over the stretch it judges, and by no
 * more, so the check is never waived whole. */
#ifndef RAY_TEST_PAGE_CACHE_H
#define RAY_TEST_PAGE_CACHE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* File pages read back after reclaim so far: this process's cgroup's
 * count (cgroup v2 memory.stat), else the system's (/proc/vmstat); -1
 * where neither is there (not Linux), and a fault budget is then judged
 * as it was. */
static inline int64_t ray_test_file_refaults(void) {
    /* path: the prefix, the cgroup's path (at most what a line holds) and
     * the file name; a path that would not fit is not opened, and the
     * system's count is read instead */
    char line[512];
    char path[sizeof "/sys/fs/cgroup" + sizeof line + sizeof "/memory.stat"] = "";
    FILE* f = fopen("/proc/self/cgroup", "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "0::", 3) == 0) {
                line[strcspn(line, "\n")] = 0;
                int w = snprintf(path, sizeof path, "/sys/fs/cgroup%s/memory.stat", line + 3);
                if (w < 0 || (size_t)w >= sizeof path) path[0] = 0;
                break;
            }
        fclose(f);
    }
    const char* srcs[2] = { path, "/proc/vmstat" };
    for (int i = 0; i < 2; i++) {
        if (!srcs[i][0] || !(f = fopen(srcs[i], "r"))) continue;
        int64_t v = -1;
        while (fgets(line, sizeof line, f)) {
            long long n;
            if (sscanf(line, "workingset_refault_file %lld", &n) == 1) { v = n; break; }
        }
        fclose(f);
        if (v >= 0) return v;
    }
    return -1;
}

#endif /* RAY_TEST_PAGE_CACHE_H */
