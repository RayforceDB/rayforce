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

#ifndef RAY_CORE_NUMPARSE_H
#define RAY_CORE_NUMPARSE_H

/* ============================================================================
 * numparse — unified (ptr, len) → value parsers
 *
 * Used by both the language tokenizer (src/lang/parse.c) and the CSV
 * reader (src/io/csv.c).  All parsers share the same shape:
 *
 *     size_t consumed = ray_parse_X(src, len, &out);
 *
 *   - returns the number of bytes consumed from `src`
 *   - 0 means "no progress" — parse failed at byte 0, *out unchanged
 *   - the language tokenizer advances its cursor by `consumed`
 *   - the CSV reader treats `consumed != len` as a null/invalid field
 *
 * No leading whitespace is stripped; callers strip first if they need to.
 * Optional sign characters (`+` / `-`) ARE consumed.
 *
 * SWAR primitives are also exported (used by fast date / time parsers
 * that consume fixed-width digit groups).
 * ============================================================================ */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t ray_parse_i64(const char *src, size_t len, int64_t  *dst);
/* ray_parse_i64, but the minimum, INT64_MIN (the I64 null), does not parse */
size_t ray_parse_i64_value(const char *src, size_t len, int64_t *dst);
size_t ray_parse_f64(const char *src, size_t len, double   *dst);
size_t ray_parse_u64_hex(const char *src, size_t len, uint64_t *dst);

/* ----------------------------------------------------------------------------
 * Float text independent of the process's LC_NUMERIC.
 *
 *   ray_strtod_c   — strtod in the C locale: reads '.' as the decimal point.
 *   ray_snprintf_c — snprintf whose output is a single number (a %g/%f/%e
 *                    conversion, optionally signed): the locale's decimal
 *                    point is written as '.'.  Returns what snprintf returns,
 *                    less the bytes a multibyte decimal point gave up.
 * ---------------------------------------------------------------------------- */

double ray_strtod_c(const char *s, char **end);
int    ray_snprintf_c(char *buf, size_t cap, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* ----------------------------------------------------------------------------
 * SWAR (SIMD Within A Register) digit primitives.
 *
 * Caller must guarantee 8 readable bytes at `p` for the 8-digit forms,
 * 4 for the 4-digit forms.  All loads are unaligned via memcpy.
 * Byte-order independent: the accumulation forms normalise the loaded
 * word to little-endian, so they are correct on big-endian hosts too.
 * ---------------------------------------------------------------------------- */

bool     ray_is_8_digits   (const void *p);
bool     ray_is_4_digits   (const void *p);
uint64_t ray_parse_8_digits(const void *p);
uint32_t ray_parse_4_digits(const void *p);

#ifdef __cplusplus
}
#endif

#endif /* RAY_CORE_NUMPARSE_H */
