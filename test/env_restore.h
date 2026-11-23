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

#ifndef RAY_TEST_ENV_RESTORE_H
#define RAY_TEST_ENV_RESTORE_H

/* The includer defines _POSIX_C_SOURCE 200809L (setenv, unsetenv, strdup,
 * strndup) before its first include, as test/main.c does. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The environment is the process's, not a test's: a test that sets a
 * variable (RAY_CSV_TRACE, TMPDIR, ...) and fails, or ends, before it puts
 * it back would hand it to every later test in this worker and to every
 * process those start.  run_one takes a copy before a test and puts the
 * environment back as it was after the teardown: variables the test added
 * removed, ones it changed or removed set again.  POSIX only. */
#if !defined(_WIN32)
extern char** environ;
static inline char** env_snapshot(void) {
    size_t n = 0;
    while (environ && environ[n]) n++;
    char** snap = (char**)calloc(n + 1, sizeof(char*));
    if (!snap) return NULL;
    for (size_t i = 0; i < n; i++) snap[i] = strdup(environ[i]);
    return snap;
}
static inline bool env_snap_has(char** snap, const char* kv) {
    size_t len = strcspn(kv, "=");
    for (size_t i = 0; snap[i]; i++)
        if (strncmp(snap[i], kv, len) == 0 && snap[i][len] == '=') return true;
    return false;
}
static inline void env_restore(char** snap) {
    if (!snap) return;
    /* Names first, then unset: unsetenv rewrites environ under the loop. */
    size_t n = 0;
    while (environ && environ[n]) n++;
    char** extra = (char**)calloc(n + 1, sizeof(char*));
    size_t k = 0;
    for (size_t i = 0; extra && i < n; i++)
        if (!env_snap_has(snap, environ[i]))
            extra[k++] = strndup(environ[i], strcspn(environ[i], "="));
    for (size_t i = 0; i < k; i++) { unsetenv(extra[i]); free(extra[i]); }
    free(extra);
    for (size_t i = 0; snap[i]; i++) {
        size_t len = strcspn(snap[i], "=");
        char* name = strndup(snap[i], len);
        const char* now = name ? getenv(name) : NULL;
        if (name && (!now || strcmp(now, snap[i] + len + 1) != 0))
            setenv(name, snap[i] + len + 1, 1);
        free(name);
        free(snap[i]);
    }
    free(snap);
}
#else
static inline char** env_snapshot(void) { return NULL; }
static inline void env_restore(char** snap) { (void)snap; }
#endif

#endif /* RAY_TEST_ENV_RESTORE_H */
