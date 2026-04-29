/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

/*
 * Rayforce test harness — zero-dependency, v1-style.
 *
 * Each test is a function returning test_result_t.  Assertions set the
 * per-thread failure buffer via FAILF() and return early.  Tests are
 * registered in a per-file test_entry_t array, exposed as an extern
 * pointer that main.c aggregates.
 *
 * For tests that drive Rayfall source (most lang-level tests), see
 * test_rfl.h for TEST_ASSERT_EQ / TEST_ASSERT_ER.
 */

#ifndef RAY_TEST_H
#define RAY_TEST_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

typedef enum { TEST_PASS = 0, TEST_FAIL, TEST_SKIP } test_status_t;

typedef struct {
    test_status_t status;
    const char*   msg;  /* NULL on PASS; diagnostic text on FAIL/SKIP */
} test_result_t;

typedef test_result_t (*test_func_t)(void);
typedef void          (*test_setup_t)(void);
typedef void          (*test_teardown_t)(void);

typedef struct {
    const char*     name;      /* test name, e.g. "atom/bool"       */
    test_func_t     func;
    test_setup_t    setup;     /* NULL = none                       */
    test_teardown_t teardown;  /* NULL = none                       */
} test_entry_t;

/* Per-thread failure-message buffer.  Overwritten on each FAIL; safe
 * because the runner prints the current test's result before starting
 * the next. */
extern char ray_test_fail_buf[2048];

/* Escape channel for helper functions that return a scalar (and so can't
 * use the early-return FAIL macros).  The runner installs a setjmp
 * target before each test and clears the flag afterward; a helper that
 * hits an unrecoverable error calls ray_test_fatal(), which fills the
 * failure buffer and longjmp's back to the runner — the test reports
 * FAIL normally, subsequent tests continue to run.  Calls from outside
 * a live test body fall through to abort(). */
extern jmp_buf ray_test_jmp;
extern int     ray_test_jmp_active;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
void ray_test_fatal(const char* fmt, ...);

#define PASS()    return (test_result_t){ TEST_PASS, NULL }
#define SKIP(msg) return (test_result_t){ TEST_SKIP, (msg) }
#define FAIL(msg) return (test_result_t){ TEST_FAIL, (msg) }

#define FAILF(fmt, ...) do {                                           \
    snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,              \
             "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__);        \
    return (test_result_t){ TEST_FAIL, ray_test_fail_buf };            \
} while (0)

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) FAILF("%s: %s", #cond, (msg));                        \
} while (0)

#define TEST_ASSERT_FMT(cond, fmt, ...) do {                           \
    if (!(cond)) FAILF(fmt, ##__VA_ARGS__);                            \
} while (0)

/* Common compound assertions. */

#define TEST_ASSERT_EQ_I(a, b) do {                                    \
    long long _a = (long long)(a), _b = (long long)(b);                \
    if (_a != _b) FAILF("%s != %s  (got %lld, expected %lld)",         \
                        #a, #b, _a, _b);                               \
} while (0)

#define TEST_ASSERT_EQ_U(a, b) do {                                    \
    unsigned long long _a = (unsigned long long)(a),                   \
                       _b = (unsigned long long)(b);                   \
    if (_a != _b) FAILF("%s != %s  (got %llu, expected %llu)",         \
                        #a, #b, _a, _b);                               \
} while (0)

#define TEST_ASSERT_EQ_F(a, b, eps) do {                               \
    double _a = (double)(a), _b = (double)(b), _e = (double)(eps);     \
    double _d = _a - _b; if (_d < 0) _d = -_d;                         \
    if (_d > _e) FAILF("%s !~ %s  (got %g, expected %g, eps %g)",      \
                       #a, #b, _a, _b, _e);                            \
} while (0)

#define TEST_ASSERT_EQ_PTR(a, b) do {                                  \
    const void* _a = (const void*)(a);                                 \
    const void* _b = (const void*)(b);                                 \
    if (_a != _b) FAILF("%s != %s  (got %p, expected %p)",             \
                        #a, #b, _a, _b);                               \
} while (0)

#define TEST_ASSERT_NOT_NULL(p) do {                                   \
    if ((p) == NULL) FAILF("%s is NULL", #p);                          \
} while (0)

#define TEST_ASSERT_NULL(p) do {                                       \
    if ((p) != NULL) FAILF("%s is not NULL", #p);                      \
} while (0)

#define TEST_ASSERT_STR_EQ(a, b) do {                                  \
    const char* _a = (a); const char* _b = (b);                        \
    if (strcmp(_a, _b) != 0)                                           \
        FAILF("strings differ: \"%s\" vs \"%s\"", _a, _b);             \
} while (0)

#define TEST_ASSERT_MEM_EQ(n, a, b) do {                               \
    size_t _n = (size_t)(n);                                           \
    if (memcmp((a), (b), _n) != 0)                                     \
        FAILF("memcmp of %zu bytes differs", _n);                      \
} while (0)

#define TEST_ASSERT_TRUE(x)  TEST_ASSERT((x), "expected true")
#define TEST_ASSERT_FALSE(x) TEST_ASSERT(!(x), "expected false")

#endif /* RAY_TEST_H */
