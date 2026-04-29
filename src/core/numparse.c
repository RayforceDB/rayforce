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

#include "core/numparse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------------------
 * SWAR digit detection
 *
 * Load 8 bytes as a little-endian u64 and use the standard Lemire trick:
 *   - subtract 0x30 from each byte → if any byte was < '0' the result
 *     underflows and the high bit of that lane is set
 *   - add 0x46 (= 0x7F - 0x39) to each byte → if any byte was > '9'
 *     the result exceeds 0x7F and the high bit of that lane is set
 *   - OR the two and mask with 0x80...80; zero ⇔ all bytes in '0'..'9'
 * ---------------------------------------------------------------------------- */

#define LANE8_BIT 0x8080808080808080ULL
#define LANE4_BIT 0x80808080U

bool ray_is_8_digits(const void *p) {
    uint64_t chunk;
    memcpy(&chunk, p, 8);
    uint64_t under = chunk - 0x3030303030303030ULL;   /* < '0' → MSB set */
    uint64_t over  = chunk + 0x4646464646464646ULL;   /* > '9' → MSB set */
    return ((under | over) & LANE8_BIT) == 0;
}

bool ray_is_4_digits(const void *p) {
    uint32_t chunk;
    memcpy(&chunk, p, 4);
    uint32_t under = chunk - 0x30303030U;
    uint32_t over  = chunk + 0x46464646U;
    return ((under | over) & LANE4_BIT) == 0;
}

/* ----------------------------------------------------------------------------
 * SWAR digit accumulation
 *
 * The classic three-stage byte-pair-quad fold from the
 * "fast atoi" literature.  Compiler folds away well at -O2/-O3, but
 * the explicit form keeps it tight at -O0 too (sanitizer build).
 * ---------------------------------------------------------------------------- */

uint64_t ray_parse_8_digits(const void *p) {
    uint64_t chunk;
    memcpy(&chunk, p, 8);
    chunk -= 0x3030303030303030ULL;                   /* now each byte ∈ 0..9 */

    /* Fold pairs of digits into 16-bit words: tens*10 + ones.  The
     * memory-low byte of each pair holds the tens digit (it printed
     * first), so on a little-endian load the tens are at chunk's even
     * bytes and the ones are at the odd bytes. */
    uint64_t tens  = chunk        & 0x000F000F000F000FULL;
    uint64_t ones  = (chunk >> 8) & 0x000F000F000F000FULL;
    uint64_t pairs = tens * 10 + ones;                /* 4 × 16-bit values 0..99 */

    /* Fold pairs-of-pairs into 32-bit words: pair_lo*100 + pair_hi,
     * where pair_lo holds the more-significant pair (printed first). */
    uint64_t p_hi   = pairs        & 0x000000FF000000FFULL;
    uint64_t p_lo   = (pairs >> 16) & 0x000000FF000000FFULL;
    uint64_t quads  = p_hi * 100 + p_lo;              /* 2 × 32-bit values 0..9999 */

    /* Final fold: low 32 bits hold the more-significant quad. */
    return (quads & 0xFFFFFFFFULL) * 10000 + (quads >> 32);
}

uint32_t ray_parse_4_digits(const void *p) {
    uint32_t chunk;
    memcpy(&chunk, p, 4);
    chunk -= 0x30303030U;
    uint32_t tens  = chunk        & 0x000F000FU;
    uint32_t ones  = (chunk >> 8) & 0x000F000FU;
    uint32_t pairs = tens * 10 + ones;                /* low 16 = pair1, high 16 = pair2 */
    return (pairs & 0xFFFFU) * 100 + (pairs >> 16);
}

/* ----------------------------------------------------------------------------
 * Integer parsers
 * ---------------------------------------------------------------------------- */

#define IS_DIGIT(c) ((unsigned)((unsigned char)(c) - '0') < 10u)

size_t ray_parse_i64(const char *src, size_t len, int64_t *dst) {
    if (len == 0) return 0;

    size_t i = 0;
    int neg = 0;
    if (src[0] == '-') { neg = 1; i = 1; }
    else if (src[0] == '+') { i = 1; }
    if (i == len) return 0;

    size_t digit_start = i;

    /* Strip leading zeros — they don't contribute to the significant
     * digit count and would otherwise force an overly strict cap below
     * (e.g. "00000000000000000001" is just 1, not a 20-digit value). */
    while (i < len && src[i] == '0') i++;
    size_t sig_start = i;

    uint64_t result = 0;

    /* SWAR: first 8 digits */
    if (i + 8 <= len && ray_is_8_digits(src + i)) {
        result = ray_parse_8_digits(src + i);
        i += 8;
        /* Second 8-digit chunk: result is in [0, 1e8), well below the
         * 922337203 bound (= u64 max ÷ 2e10) that keeps result*1e8 +
         * 1e8-1 from wrapping u64. */
        if (i + 8 <= len && result <= 922337203ULL && ray_is_8_digits(src + i)) {
            result = result * 100000000ULL + ray_parse_8_digits(src + i);
            i += 8;
        }
    }

    /* Scalar tail with strict 19-digit cap.  INT64_MAX (and |INT64_MIN|)
     * have 19 decimal digits; anything past that always overflows i64
     * and may also overflow u64 in a way where the wrapped value lands
     * back inside [0, INT64_MAX], silently misparsing oversized inputs
     * as small in-range values.  Cut off before that can happen. */
    while (i < len && IS_DIGIT(src[i])) {
        if ((size_t)(i - sig_start) >= 19) return 0; /* too many sig digits */
        uint64_t prev = result;
        result = result * 10 + (uint64_t)(src[i] - '0');
        if (result < prev) return 0;                 /* u64 wrap (defensive) */
        i++;
    }

    if (i == digit_start) return 0;                  /* no digits at all */

    /* Fit into int64 with proper handling of INT64_MIN. */
    if (neg) {
        if (result > (uint64_t)INT64_MAX + 1ULL) return 0;
        *dst = (int64_t)(0u - result);                /* avoids signed UB */
    } else {
        if (result > (uint64_t)INT64_MAX) return 0;
        *dst = (int64_t)result;
    }
    return i;
}

size_t ray_parse_i32(const char *src, size_t len, int32_t *dst) {
    int64_t v;
    size_t n = ray_parse_i64(src, len, &v);
    if (n == 0) return 0;
    if (v < INT32_MIN || v > INT32_MAX) return 0;
    *dst = (int32_t)v;
    return n;
}

/* ----------------------------------------------------------------------------
 * Float parser
 *
 * Layout: [+-]digits[.digits][eE[+-]digits]
 * Also accepts NaN, Inf, +Inf, -Inf (case-insensitive prefix; we match
 * the same forms the language printer emits and that .csv.write produces).
 * ---------------------------------------------------------------------------- */

static const double g_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static inline int icmp3(const char *p, char a, char b, char c) {
    unsigned char x = (unsigned char)p[0], y = (unsigned char)p[1], z = (unsigned char)p[2];
    return (x == (unsigned char)a || x == (unsigned char)(a ^ 0x20)) &&
           (y == (unsigned char)b || y == (unsigned char)(b ^ 0x20)) &&
           (z == (unsigned char)c || z == (unsigned char)(c ^ 0x20));
}

/* Apply 10^e to val.
 *
 * For |e| ≤ 22 the pow10 table entries are exact f64 (10^k for k ≤ 22 is
 * representable), so a single multiply / divide is correctly rounded.
 *
 * For positive e > 22 we use libm `pow(10, e)` and a *single* multiply.
 * Chaining `val *= 1e22` instead would accumulate ~½ ulp per step and
 * thirteen steps is enough to push values right at the DBL_MAX boundary
 * (e.g. 1.7976931348623158e308) over the rounding threshold into +inf,
 * even though the correctly-rounded f64 is still finite.
 *
 * For negative e, single multiply via `pow(10, -324)` would underflow to
 * zero before the multiply could lift the result back into the denormal
 * range — so 2.2250738585072014e-308 becomes 0.  We chain by 1e22 in
 * that direction; chained division stays well-conditioned all the way
 * down to the smallest denormal. */
static inline double scale_pow10(double val, int e) {
    if (e == 0) return val;
    if (e > 0) {
        if (e <= 22) return val * g_pow10[e];
        return val * pow(10.0, (double)e);
    } else {
        int ne = -e;
        if (ne <= 22) return val / g_pow10[ne];
        while (ne > 22) {
            val /= 1e22;
            if (val == 0.0) return val;
            ne -= 22;
        }
        return val / g_pow10[ne];
    }
}

size_t ray_parse_f64(const char *src, size_t len, double *dst) {
    if (len == 0) return 0;

    size_t i = 0;
    int neg = 0;
    if (src[0] == '-') { neg = 1; i = 1; }
    else if (src[0] == '+') { i = 1; }

    /* NaN / Inf */
    if (i + 3 <= len && icmp3(src + i, 'n', 'a', 'n')) {
        *dst = __builtin_nan("");
        return i + 3;
    }
    if (i + 3 <= len && icmp3(src + i, 'i', 'n', 'f')) {
        *dst = neg ? -__builtin_inf() : __builtin_inf();
        return i + 3;
    }

    if (i == len) return 0;

    /* Build a single decimal mantissa in u64 plus a signed power-of-ten
     * offset, then finalize with one multiply.  This avoids two pitfalls
     * the earlier hand-rolled accumulator had:
     *
     *   1. A purely positional fractional cap dropped meaningful trailing
     *      digits when leading zeros took up the budget — so 1e-19 written
     *      as "0.0000000000000000001" came back as 0.
     *   2. Chained `val *= 1e22` for large exponents accumulated rounding
     *      error past DBL_MAX, turning DBL_MAX itself into inf.
     */
    uint64_t mantissa = 0;
    int      mant_digits = 0;          /* significant digits captured */
    int      dec_offset  = 0;          /* power of 10 to apply at the end */
    bool     have_digit  = false;

    /* ---- integer part ----------------------------------------------- */

    /* Skip leading zeros (don't count as significant). */
    while (i < len && src[i] == '0') { i++; have_digit = true; }

    /* SWAR fast path for the first 8 / 16 sig digits. */
    if (i + 8 <= len && ray_is_8_digits(src + i)) {
        mantissa = ray_parse_8_digits(src + i);
        mant_digits = 8;
        i += 8;
        have_digit = true;
        if (i + 8 <= len && ray_is_8_digits(src + i)) {
            mantissa = mantissa * 100000000ULL + ray_parse_8_digits(src + i);
            mant_digits = 16;
            i += 8;
        }
    }

    /* Scalar tail of the integer part.  Past 18 sig digits we drop
     * further integer digits but keep their magnitude via dec_offset. */
    while (i < len && IS_DIGIT(src[i])) {
        if (mant_digits < 18) {
            mantissa = mantissa * 10 + (uint64_t)(src[i] - '0');
            mant_digits++;
        } else {
            dec_offset++;
        }
        i++;
        have_digit = true;
    }

    /* ---- fractional part -------------------------------------------- */

    if (i < len && src[i] == '.') {
        i++;
        /* Leading zeros in the fractional part (when the mantissa is
         * still 0) shift the decimal point but contribute no significant
         * digit. */
        if (mantissa == 0) {
            while (i < len && src[i] == '0') {
                dec_offset--;
                i++;
                have_digit = true;
            }
        }

        /* SWAR fast path for the first 8 sig fractional digits. */
        if (i + 8 <= len && mant_digits + 8 <= 18 && ray_is_8_digits(src + i)) {
            mantissa = mantissa * 100000000ULL + ray_parse_8_digits(src + i);
            mant_digits += 8;
            dec_offset -= 8;
            i += 8;
            have_digit = true;
        }

        /* Scalar tail of the fractional part.  Past 18 sig digits we
         * skip further fractional digits — they are below f64 precision
         * and they don't shift the magnitude (no dec_offset change). */
        while (i < len && IS_DIGIT(src[i])) {
            if (mant_digits < 18) {
                mantissa = mantissa * 10 + (uint64_t)(src[i] - '0');
                mant_digits++;
                dec_offset--;
            }
            i++;
            have_digit = true;
        }
    }

    if (!have_digit) return 0;

    /* ---- explicit exponent ------------------------------------------ */

    if (i < len && (src[i] == 'e' || src[i] == 'E')) {
        size_t e_at = i;
        i++;
        int e_neg = 0;
        if (i < len) {
            if (src[i] == '-') { e_neg = 1; i++; }
            else if (src[i] == '+') { i++; }
        }
        size_t e_start = i;
        int exp_v = 0;
        bool exp_capped = false;
        while (i < len && IS_DIGIT(src[i])) {
            if (exp_v <= 999) exp_v = exp_v * 10 + (src[i] - '0');
            else exp_capped = true;
            i++;
        }
        if (i == e_start) {
            /* "1e" with no digits — rewind; the 'e' is not part of the number. */
            i = e_at;
        } else {
            int e = exp_capped ? 10000 : exp_v;
            dec_offset += e_neg ? -e : e;
        }
    }

    /* ---- finalize: val = mantissa * 10^dec_offset ------------------- */

    /* Fast path applies only when the conversion is provably correctly
     * rounded — i.e. both factors of the final multiply are exact f64s:
     *
     *   - (double)mantissa is exact for mantissa ≤ 2^53.  Significant
     *     digits ≤ 15 keeps mantissa ≤ 10^15 - 1 < 2^53.
     *   - g_pow10[|k|] is exact for |k| ≤ 22 (10^22 fits in 76 bits but
     *     IEEE 754 happens to round 10^k for k ≤ 22 to a value that
     *     matches the table entries we hand-wrote).
     *
     * Outside that window — high-precision mantissas, large exponents,
     * or boundary-near values — defer to libc strtod on the original
     * substring.  glibc strtod is correctly rounded, so this fixes:
     *   • DBL_MAX-edge overshoot (1.7976931348623158e308 → +inf in the
     *     fast path; strtod rounds to DBL_MAX);
     *   • DBL_MAX_PREV mismatch (1.7976931348623155e308 — fast path
     *     gives DBL_MAX, strtod correctly gives DBL_MAX_PREV);
     *   • Denormal underflow (mantissa·pow(10,-324) zeroes out before
     *     scale_pow10's chained division could keep the result alive).
     *
     * Most CSV / lang values land in the fast path: they have ≤ 15
     * significant digits and modest exponents.  The slow lane is
     * reserved for inputs where the trade-off is correctness over
     * speed. */
    double val = 0.0;
    bool   need_strtod = false;

    if (mantissa == 0) {
        val = 0.0;
    } else if (dec_offset > 308) {
        val = __builtin_inf();
    } else if (dec_offset < -342) {                  /* below denormal range */
        val = 0.0;
    } else if (mant_digits <= 15 && dec_offset >= -22 && dec_offset <= 22) {
        val = (double)mantissa;
        if (dec_offset > 0)      val *= g_pow10[dec_offset];
        else if (dec_offset < 0) val /= g_pow10[-dec_offset];
    } else {
        need_strtod = true;
    }

    if (need_strtod) {
        char stackbuf[128];
        char *buf = (i + 1 <= sizeof(stackbuf)) ? stackbuf : malloc(i + 1);
        if (buf) {
            memcpy(buf, src, i);
            buf[i] = '\0';
            char *endp = NULL;
            double v = strtod(buf, &endp);
            bool ok = (endp == buf + i);
            if (buf != stackbuf) free(buf);
            if (ok) {
                /* strtod already applied the leading sign in buf, so
                 * don't apply `neg` again. */
                *dst = v;
                return i;
            }
        }
        /* Strtod unusable (OOM on a giant literal, or unexpected parse
         * disagreement).  Fall through with the approximate result
         * from the chained-multiply slow path so we still return a
         * sensible value rather than 0. */
        val = scale_pow10((double)mantissa, dec_offset);
    }

    *dst = neg ? -val : val;
    return i;
}

/* ----------------------------------------------------------------------------
 * Hexadecimal (no 0x prefix, lowercase or uppercase)
 * ---------------------------------------------------------------------------- */

size_t ray_parse_u64_hex(const char *src, size_t len, uint64_t *dst) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < len && i < 16) {
        unsigned char c = (unsigned char)src[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        i++;
    }
    if (i == 0) return 0;
    *dst = v;
    return i;
}
