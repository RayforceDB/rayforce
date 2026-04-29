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

#include "test.h"

#include "core/numparse.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ─── SWAR primitives ────────────────────────────────────────────────── */

static test_result_t test_numparse_swar_8digits(void) {
    /* All digits */
    TEST_ASSERT_TRUE(ray_is_8_digits("00000000"));
    TEST_ASSERT_TRUE(ray_is_8_digits("12345678"));
    TEST_ASSERT_TRUE(ray_is_8_digits("99999999"));
    /* Boundary chars: '/' (0x2F) is < '0', ':' (0x3A) is > '9'. */
    TEST_ASSERT_FALSE(ray_is_8_digits("/0000000"));
    TEST_ASSERT_FALSE(ray_is_8_digits("0000000/"));
    TEST_ASSERT_FALSE(ray_is_8_digits(":0000000"));
    TEST_ASSERT_FALSE(ray_is_8_digits("0000000:"));
    TEST_ASSERT_FALSE(ray_is_8_digits("12-45678"));

    TEST_ASSERT_EQ_U(ray_parse_8_digits("12345678"), 12345678ULL);
    TEST_ASSERT_EQ_U(ray_parse_8_digits("00000001"), 1ULL);
    TEST_ASSERT_EQ_U(ray_parse_8_digits("99999999"), 99999999ULL);
    TEST_ASSERT_EQ_U(ray_parse_8_digits("00000000"), 0ULL);
    PASS();
}

static test_result_t test_numparse_swar_4digits(void) {
    TEST_ASSERT_TRUE(ray_is_4_digits("2024"));
    TEST_ASSERT_FALSE(ray_is_4_digits("20-4"));
    TEST_ASSERT_FALSE(ray_is_4_digits("/000"));
    TEST_ASSERT_FALSE(ray_is_4_digits("000:"));

    TEST_ASSERT_EQ_U((uint64_t)ray_parse_4_digits("2024"), 2024U);
    TEST_ASSERT_EQ_U((uint64_t)ray_parse_4_digits("0000"), 0U);
    TEST_ASSERT_EQ_U((uint64_t)ray_parse_4_digits("9999"), 9999U);
    PASS();
}

/* ─── ray_parse_i64 ─────────────────────────────────────────────────── */

#define ASSERT_I64(s, want_n, want_v) do {                                 \
    int64_t _v = 0;                                                        \
    size_t _n = ray_parse_i64((s), strlen(s), &_v);                        \
    TEST_ASSERT_FMT(_n == (size_t)(want_n),                                \
        "i64(\"%s\"): consumed %zu, want %zu", (s), _n, (size_t)(want_n)); \
    if (_n) {                                                              \
        TEST_ASSERT_FMT(_v == (int64_t)(want_v),                           \
            "i64(\"%s\"): got %lld, want %lld",                            \
            (s), (long long)_v, (long long)(want_v));                      \
    }                                                                      \
} while (0)

static test_result_t test_numparse_i64_basic(void) {
    ASSERT_I64("0",          1,  0);
    ASSERT_I64("1",          1,  1);
    ASSERT_I64("12345",      5,  12345);
    ASSERT_I64("-1",         2, -1);
    ASSERT_I64("+42",        3,  42);
    ASSERT_I64("00000123",   8,  123);                /* leading zeros */
    PASS();
}

static test_result_t test_numparse_i64_swar_paths(void) {
    /* exactly 8 digits → fast path 1 */
    ASSERT_I64("12345678",         8,  12345678LL);
    /* exactly 16 digits → fast path 1+2 */
    ASSERT_I64("1234567890123456", 16, 1234567890123456LL);
    /* 17 digits → fast 8 + 8 if first chunk small enough, else fast 8 + scalar */
    ASSERT_I64("12345678901234567", 17, 12345678901234567LL);
    /* 18 digits, just inside u64 */
    ASSERT_I64("123456789012345678", 18, 123456789012345678LL);
    /* INT64_MAX = 9223372036854775807 (19 digits) */
    ASSERT_I64("9223372036854775807", 19, INT64_MAX);
    /* INT64_MIN = -9223372036854775808 */
    ASSERT_I64("-9223372036854775808", 20, INT64_MIN);
    PASS();
}

static test_result_t test_numparse_i64_overflow(void) {
    /* Overflow → 0 (no progress).  Lang parser reads this as "promote to f64". */
    int64_t v = 0;
    TEST_ASSERT_EQ_U(ray_parse_i64("9223372036854775808", 19, &v), 0); /* INT64_MAX + 1 */
    TEST_ASSERT_EQ_U(ray_parse_i64("99999999999999999999", 20, &v), 0); /* > u64 */
    TEST_ASSERT_EQ_U(ray_parse_i64("-9223372036854775809", 20, &v), 0); /* INT64_MIN - 1 */
    PASS();
}

/* Regression: 20-digit values where result*10 wraps u64 by exactly one
 * round can land back inside [0, INT64_MAX].  Before the 19-digit cap
 * neither the `result < prev` check nor the final fit check noticed,
 * so 25000000000000000000 (2.5e19) silently became 6553255926290448384.
 * These inputs must all overflow. */
static test_result_t test_numparse_i64_silent_wrap(void) {
    int64_t v = 0;
    TEST_ASSERT_EQ_U(ray_parse_i64("25000000000000000000", 20, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_i64("26000000000000000000", 20, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_i64("27000000000000000000", 20, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_i64("-25000000000000000000", 21, &v), 0);
    /* 1e19 = 10000000000000000000 — fits in u64 but exceeds INT64_MAX. */
    TEST_ASSERT_EQ_U(ray_parse_i64("10000000000000000000", 20, &v), 0);
    PASS();
}

static test_result_t test_numparse_i64_leading_zeros(void) {
    /* Leading zeros must not push small values past the 19-digit cap. */
    ASSERT_I64("00000000000000000001",      20, 1LL);
    ASSERT_I64("000000000000000000000000", 24, 0LL);
    ASSERT_I64("-0000000000000000000042",   23, -42LL);
    /* But significant digits past 19 must still overflow regardless of
     * how many leading zeros precede them. */
    int64_t v = 0;
    TEST_ASSERT_EQ_U(
        ray_parse_i64("0000000000000000099999999999999999999",
                      37, &v), 0);
    PASS();
}

static test_result_t test_numparse_i64_partial(void) {
    /* Lang-parser style: stop at first non-digit, return bytes consumed. */
    int64_t v = 0;
    TEST_ASSERT_EQ_U(ray_parse_i64("123abc", 6, &v), 3);
    TEST_ASSERT_EQ_I(v, 123);
    TEST_ASSERT_EQ_U(ray_parse_i64("42 ", 3, &v), 2);
    TEST_ASSERT_EQ_I(v, 42);
    /* Empty / no digits */
    TEST_ASSERT_EQ_U(ray_parse_i64("", 0, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_i64("abc", 3, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_i64("-", 1, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_i64("+", 1, &v), 0);
    PASS();
}

/* ─── ray_parse_f64 ─────────────────────────────────────────────────── */

#define ASSERT_F64(s, want_n, want_v) do {                                 \
    double _v = 0.0;                                                       \
    size_t _n = ray_parse_f64((s), strlen(s), &_v);                        \
    TEST_ASSERT_FMT(_n == (size_t)(want_n),                                \
        "f64(\"%s\"): consumed %zu, want %zu", (s), _n, (size_t)(want_n)); \
    if (_n) TEST_ASSERT_EQ_F(_v, (double)(want_v), 1e-9);                  \
} while (0)

static test_result_t test_numparse_f64_basic(void) {
    ASSERT_F64("0",        1,  0.0);
    ASSERT_F64("1.5",      3,  1.5);
    ASSERT_F64("-1.5",     4, -1.5);
    ASSERT_F64("+0.25",    5,  0.25);
    ASSERT_F64(".5",       2,  0.5);                  /* leading dot */
    ASSERT_F64("123.",     4,  123.0);                /* trailing dot */
    ASSERT_F64("1e10",     4,  1e10);
    ASSERT_F64("1.5e-3",   6,  1.5e-3);
    ASSERT_F64("1.5E+3",   6,  1500.0);
    PASS();
}

static test_result_t test_numparse_f64_special(void) {
    double v = 0.0;
    TEST_ASSERT_EQ_U(ray_parse_f64("nan", 3, &v), 3);
    TEST_ASSERT_TRUE(isnan(v));
    TEST_ASSERT_EQ_U(ray_parse_f64("NaN", 3, &v), 3);
    TEST_ASSERT_TRUE(isnan(v));
    TEST_ASSERT_EQ_U(ray_parse_f64("inf", 3, &v), 3);
    TEST_ASSERT_TRUE(isinf(v) && v > 0);
    TEST_ASSERT_EQ_U(ray_parse_f64("+Inf", 4, &v), 4);
    TEST_ASSERT_TRUE(isinf(v) && v > 0);
    TEST_ASSERT_EQ_U(ray_parse_f64("-INF", 4, &v), 4);
    TEST_ASSERT_TRUE(isinf(v) && v < 0);
    PASS();
}

static test_result_t test_numparse_f64_partial(void) {
    double v = 0.0;
    TEST_ASSERT_EQ_U(ray_parse_f64("1.5xyz", 6, &v), 3);
    TEST_ASSERT_EQ_F(v, 1.5, 1e-9);
    /* "1e" with no digits — rewinds, only "1" consumed */
    TEST_ASSERT_EQ_U(ray_parse_f64("1e", 2, &v), 1);
    TEST_ASSERT_EQ_F(v, 1.0, 1e-9);
    /* Empty */
    TEST_ASSERT_EQ_U(ray_parse_f64("", 0, &v), 0);
    /* Sign only */
    TEST_ASSERT_EQ_U(ray_parse_f64("-", 1, &v), 0);
    TEST_ASSERT_EQ_U(ray_parse_f64("+", 1, &v), 0);
    PASS();
}

/* Regression: previous chained `val *= 1e22` over-shot DBL_MAX, mapping
 * the literal value of DBL_MAX to inf.  Each assertion checks the exact
 * f64 value (via DBL_MAX equality or strtod-as-reference) — a "v is
 * finite" check would also accept a fallback that silently returned 0
 * or any other in-range value, which is not the property we're
 * defending. */
static test_result_t test_numparse_f64_dbl_max(void) {
    double v = 0.0;

    /* Canonical DBL_MAX literal — must round to exactly DBL_MAX. */
    size_t n = ray_parse_f64("1.7976931348623157e308", 22, &v);
    TEST_ASSERT_EQ_U(n, 22);
    TEST_ASSERT_FMT(v == DBL_MAX,
        "DBL_MAX literal: got %.17g, want %.17g", v, DBL_MAX);

    /* One ulp above DBL_MAX in the literal but within the round-to-
     * nearest band that maps to DBL_MAX (strtod agrees).  Earlier
     * `mantissa * pow(10, 292)` carried 1 ulp of rounding error and
     * tipped over to +inf — the fix routes this through the strtod
     * fallback. */
    n = ray_parse_f64("1.7976931348623158e308", 22, &v);
    TEST_ASSERT_EQ_U(n, 22);
    TEST_ASSERT_FMT(v == DBL_MAX,
        "1.7976931348623158e308: got %.17g, want %.17g", v, DBL_MAX);

    /* DBL_MAX_PREV literal must round to DBL_MAX_PREV exactly (not
     * DBL_MAX 1 ulp away).  The earlier fast path computed
     * `(double)17976931348623155 * pow(10, 292)` and silently rounded
     * up to DBL_MAX; now any input with > 15 sig digits or |dec_offset|
     * > 22 routes through strtod for correct rounding. */
    {
        double ref = strtod("1.7976931348623155e308", NULL);
        n = ray_parse_f64("1.7976931348623155e308", 22, &v);
        TEST_ASSERT_EQ_U(n, 22);
        TEST_ASSERT_FMT(v == ref,
            "1.7976931348623155e308: got %.17g, want %.17g", v, ref);
    }

    /* Genuinely above the rounding threshold → inf is correct. */
    n = ray_parse_f64("1.8e308", 7, &v);
    TEST_ASSERT_EQ_U(n, 7);
    TEST_ASSERT_TRUE(isinf(v));

    /* Long-literal case.  To actually exercise the strtod fall-back we
     * need an input that produces the same (mantissa, dec_offset) as
     * the short-form boundary value (17976931348623158 × 10^292) — that
     * is the combination whose single multiply tips into +inf.  Padding
     * the *fractional* part with zeros would shift the mantissa by one
     * digit (179769313486231580 × 10^291) and route around the overshoot,
     * so a frac-padded test would pass with or without the fix.  Leading
     * *integer* zeros are skipped before the significant-digit count
     * starts, so they preserve the boundary decomposition while making
     * the input arbitrarily long — past the on-stack 128-byte buffer
     * fast path.  Asserting exact DBL_MAX equality also guards against
     * a "lossy" canonical-rebuild fallback that drops digits past
     * position 18.
     */
    static char long_lit[2048];
    memset(long_lit, '0', 1500);
    memcpy(long_lit + 1500, "1.7976931348623158e308", 22);
    long_lit[1522] = '\0';
    size_t llen = 1522;
    n = ray_parse_f64(long_lit, llen, &v);
    TEST_ASSERT_EQ_U(n, llen);
    TEST_ASSERT_FMT(v == DBL_MAX,
        "long DBL_MAX (1522 chars): got %.17g, want %.17g", v, DBL_MAX);
    PASS();
}

/* Regression: a positional cap on fractional digits silently dropped the
 * single significant digit when there were 18+ leading zeros, mapping
 * 1e-19 written as "0.0000000000000000001" to 0. */
static test_result_t test_numparse_f64_tiny_leading_zeros(void) {
    double v = 0.0;
    size_t n = ray_parse_f64("0.0000000000000000001", 21, &v);
    TEST_ASSERT_EQ_U(n, 21);
    TEST_ASSERT_TRUE(v != 0.0);
    /* 1e-19 to within ~1 ulp. */
    TEST_ASSERT_EQ_F(v, 1e-19, 1e-34);
    /* Smallest positive normal double — must not underflow on the way
     * down. */
    n = ray_parse_f64("2.2250738585072014e-308", 23, &v);
    TEST_ASSERT_EQ_U(n, 23);
    TEST_ASSERT_TRUE(v != 0.0);
    PASS();
}

static test_result_t test_numparse_f64_swar(void) {
    /* 8-digit + 8-digit fast paths in integer part */
    ASSERT_F64("12345678",         8,  12345678.0);
    ASSERT_F64("1234567812345678", 16, 1.234567812345678e15);
    /* SWAR fast path in fractional part */
    ASSERT_F64("0.12345678",       10, 0.12345678);
    /* Long fractional digits beyond f64 precision should still parse. */
    {
        double v = 0.0;
        size_t n = ray_parse_f64("0.123456789012345678901234567890", 32, &v);
        TEST_ASSERT_EQ_U(n, 32);
        TEST_ASSERT_EQ_F(v, 0.12345678901234568, 1e-15);
    }
    PASS();
}

/* ─── ray_parse_u64_hex ─────────────────────────────────────────────── */

static test_result_t test_numparse_hex(void) {
    uint64_t v = 0;
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("0", 1, &v), 1);
    TEST_ASSERT_EQ_U(v, 0ULL);
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("ff", 2, &v), 2);
    TEST_ASSERT_EQ_U(v, 0xFFULL);
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("DEADBEEF", 8, &v), 8);
    TEST_ASSERT_EQ_U(v, 0xDEADBEEFULL);
    /* Mixed case */
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("AbCdEf", 6, &v), 6);
    TEST_ASSERT_EQ_U(v, 0xABCDEFULL);
    /* Stops at non-hex */
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("12gh", 4, &v), 2);
    TEST_ASSERT_EQ_U(v, 0x12ULL);
    /* Caps at 16 digits */
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("FFFFFFFFFFFFFFFF1", 17, &v), 16);
    TEST_ASSERT_EQ_U(v, UINT64_MAX);
    /* Empty input */
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("", 0, &v), 0);
    /* No hex digits */
    TEST_ASSERT_EQ_U(ray_parse_u64_hex("xyz", 3, &v), 0);
    PASS();
}

const test_entry_t numparse_entries[] = {
    { "numparse/swar_8digits",   test_numparse_swar_8digits, NULL, NULL },
    { "numparse/swar_4digits",   test_numparse_swar_4digits, NULL, NULL },
    { "numparse/i64_basic",      test_numparse_i64_basic,    NULL, NULL },
    { "numparse/i64_swar_paths", test_numparse_i64_swar_paths, NULL, NULL },
    { "numparse/i64_overflow",   test_numparse_i64_overflow, NULL, NULL },
    { "numparse/i64_silent_wrap",test_numparse_i64_silent_wrap, NULL, NULL },
    { "numparse/i64_leading_zeros",test_numparse_i64_leading_zeros, NULL, NULL },
    { "numparse/i64_partial",    test_numparse_i64_partial,  NULL, NULL },
    { "numparse/f64_basic",      test_numparse_f64_basic,    NULL, NULL },
    { "numparse/f64_special",    test_numparse_f64_special,  NULL, NULL },
    { "numparse/f64_partial",    test_numparse_f64_partial,  NULL, NULL },
    { "numparse/f64_swar",       test_numparse_f64_swar,     NULL, NULL },
    { "numparse/f64_dbl_max",    test_numparse_f64_dbl_max,  NULL, NULL },
    { "numparse/f64_tiny_leading_zeros",
                                 test_numparse_f64_tiny_leading_zeros, NULL, NULL },
    { "numparse/hex",            test_numparse_hex,          NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
