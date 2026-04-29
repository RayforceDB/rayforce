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
 * Rayfall-source assertions — for tests that drive the language via
 * ray_eval_str().  Mirrors v1's TEST_ASSERT_EQ/TEST_ASSERT_ER pattern:
 * both operands are Rayfall source strings, evaluated and compared by
 * formatted output.
 */

#ifndef RAY_TEST_RFL_H
#define RAY_TEST_RFL_H

#include "test.h"
#include <rayforce.h>
#include "lang/eval.h"
#include "lang/format.h"

/* Evaluate `lhs` and `rhs` as Rayfall source, format each result, and
 * string-compare the formatted forms.  Fails on eval error on either
 * side, or when the formatted strings differ.  Matches v1 semantics. */
#define TEST_ASSERT_EQ(lhs, rhs) do {                                       \
    ray_t* _le = ray_eval_str(lhs);                                         \
    if (!_le || RAY_IS_ERR(_le)) {                                          \
        ray_t* _es = _le ? ray_fmt(_le, 0) : NULL;                          \
        /* capture the error string, then reclaim the error before FAILF */ \
        char _em[512]; size_t _eml = _es ? ray_str_len(_es) : 0;            \
        if (_eml > sizeof _em - 1) _eml = sizeof _em - 1;                   \
        if (_es) { memcpy(_em, ray_str_ptr(_es), _eml); ray_release(_es); } \
        _em[_eml] = '\0';                                                   \
        if (_le) ray_error_free(_le);                                       \
        FAILF("eval error on %s: %s", (lhs), _em);                          \
    }                                                                       \
    ray_t* _re = ray_eval_str(rhs);                                         \
    if (!_re || RAY_IS_ERR(_re)) {                                          \
        ray_t* _es = _re ? ray_fmt(_re, 0) : NULL;                          \
        char _em[512]; size_t _eml = _es ? ray_str_len(_es) : 0;            \
        if (_eml > sizeof _em - 1) _eml = sizeof _em - 1;                   \
        if (_es) { memcpy(_em, ray_str_ptr(_es), _eml); ray_release(_es); } \
        _em[_eml] = '\0';                                                   \
        ray_release(_le);                                                   \
        if (_re) ray_error_free(_re);                                       \
        FAILF("eval error on RHS %s: %s", (rhs), _em);                      \
    }                                                                       \
    ray_t* _ls = ray_fmt(_le, 0);                                           \
    ray_t* _rs = ray_fmt(_re, 0);                                           \
    const char* _lp = _ls ? ray_str_ptr(_ls) : "";                          \
    const char* _rp = _rs ? ray_str_ptr(_rs) : "";                          \
    size_t _ll = _ls ? ray_str_len(_ls) : 0;                                \
    size_t _rl = _rs ? ray_str_len(_rs) : 0;                                \
    int _same = (_ll == _rl) && (memcmp(_lp, _rp, _rl) == 0);               \
    if (!_same) {                                                           \
        char _fb[2048];                                                     \
        snprintf(_fb, sizeof _fb,                                           \
                 "expected \"%.*s\", got \"%.*s\"  -- expr: %s",            \
                 (int)_rl, _rp, (int)_ll, _lp, (lhs));                      \
        if (_ls) ray_release(_ls);                                          \
        if (_rs) ray_release(_rs);                                          \
        ray_release(_le); ray_release(_re);                                 \
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,               \
                 "%s:%d: %s", __FILE__, __LINE__, _fb);                     \
        return (test_result_t){ TEST_FAIL, ray_test_fail_buf };             \
    }                                                                       \
    if (_ls) ray_release(_ls);                                              \
    if (_rs) ray_release(_rs);                                              \
    ray_release(_le); ray_release(_re);                                     \
} while (0)

/* Evaluate `src`, expect an error whose formatted message contains
 * `substr`.  Fails if eval succeeds or the message doesn't contain the
 * substring. */
#define TEST_ASSERT_ER(src, substr) do {                                    \
    ray_t* _le = ray_eval_str(src);                                         \
    if (!_le || !RAY_IS_ERR(_le)) {                                         \
        /* _le is a VALUE here (not an error) — ordinary ray_release. */    \
        ray_t* _s = _le ? ray_fmt(_le, 0) : NULL;                           \
        char _em[512]; size_t _eml = _s ? ray_str_len(_s) : 0;              \
        if (_eml > sizeof _em - 1) _eml = sizeof _em - 1;                   \
        if (_s) { memcpy(_em, ray_str_ptr(_s), _eml); ray_release(_s); }    \
        _em[_eml] = '\0';                                                   \
        if (_le) ray_release(_le);                                          \
        FAILF("expected error containing \"%s\", got: %s", (substr), _em);  \
    }                                                                       \
    /* _le IS an error beyond this point — reclaim via ray_error_free. */   \
    ray_t* _s = ray_fmt(_le, 0);                                            \
    const char* _sp = _s ? ray_str_ptr(_s) : "";                            \
    int _hit = strstr(_sp, (substr)) != NULL;                               \
    char _fb[2048] = "";                                                    \
    if (!_hit) {                                                            \
        snprintf(_fb, sizeof _fb,                                           \
                 "error \"%s\" missing substring \"%s\"  -- expr: %s",      \
                 _sp, (substr), (src));                                     \
    }                                                                       \
    if (_s) ray_release(_s);                                                \
    ray_error_free(_le);                                                    \
    if (!_hit) {                                                            \
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,               \
                 "%s:%d: %s", __FILE__, __LINE__, _fb);                     \
        return (test_result_t){ TEST_FAIL, ray_test_fail_buf };             \
    }                                                                       \
} while (0)

#endif /* RAY_TEST_RFL_H */
