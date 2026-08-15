/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Shared scaffolding for the coverage-guided fuzz drivers.
 *
 *   Every driver is a libFuzzer-style translation unit exporting
 *   LLVMFuzzerTestOneInput().  The engine calls it in a tight loop with
 *   engine-mutated byte buffers; a crash, sanitizer trip, timeout, or
 *   OOM is a finding.  These drivers live under fuzz/ (outside the src
 *   tree the Makefile globs), so they never link into the product.
 *
 *   Determinism: fuzzing wants the same input to hit the same code path
 *   every run, so we pin the worker pool to serial (RAYFORCE_CORES=0) and
 *   create the runtime exactly once, lazily, on the first input.
 */

#ifndef RAY_FUZZ_COMMON_H
#define RAY_FUZZ_COMMON_H

/* setenv() is POSIX, not ISO C — same feature-test macro the rest of the
 * tree uses (see src/core/ipc.c, src/store/journal.c). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rayforce.h>

/* Runtime lifecycle is part of the public header; fuzz drivers only need
 * create(). */

/* Lazy one-time runtime init.  libFuzzer offers LLVMFuzzerInitialize, but
 * a guarded lazy path keeps each driver self-contained and also works when
 * a driver is reused as a plain `main` replay harness. */
static int ray_fuzz_ready = 0;

static inline void ray_fuzz_init(void) {
    if (ray_fuzz_ready) return;
    /* Serial execution: deterministic coverage, no worker-thread noise.
     * setenv before runtime_create so ray_pool_create picks it up. */
    setenv("RAYFORCE_CORES", "0", 1);
    ray_runtime_create(0, NULL);
    ray_fuzz_ready = 1;
}

/* NUL-terminate an arbitrary byte range into a heap buffer the caller owns.
 * Fuzz inputs are not NUL-terminated; the parser wants a C string. */
static inline char* ray_fuzz_cstr(const uint8_t* data, size_t size) {
    char* s = (char*)malloc(size + 1);
    if (!s) return NULL;
    if (size) memcpy(s, data, size);
    s[size] = '\0';
    return s;
}

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

/* Materialise the input as an anonymous in-memory file and hand back a
 * /proc/self/fd path for the path-based readers (CSV, journal).  No disk
 * I/O, no cleanup races.  Returns the fd (>=0) and fills path_out with the
 * procfs path; the caller closes the fd when done.  Returns -1 on failure. */
static inline int ray_fuzz_memfd(const uint8_t* data, size_t size,
                                 char* path_out, size_t path_cap) {
    int fd = memfd_create("rayfuzz", 0);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    snprintf(path_out, path_cap, "/proc/self/fd/%d", fd);
    return fd;
}

#endif /* RAY_FUZZ_COMMON_H */
