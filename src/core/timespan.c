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

#include "core/timespan.h"
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>

#define DAY_NS UINT64_C(86400000000000)

static bool two_digits(const char** p, unsigned* out) {
    const char* s = *p;
    if (s[0] < '0' || s[0] > '9') return false;
    if (s[1] < '0' || s[1] > '9') return false;
    *out = (unsigned)(s[0] - '0') * 10 + (unsigned)(s[1] - '0');
    *p += 2;
    return true;
}

int ray_parse_timespan(const char* text, size_t* consumed, int64_t* value) {
    const char* p = text;
    bool negative = *p == '-';
    if (negative) p++;
    if (p[0] == '0' && p[1] == 'N' && p[2] == 'n' && !negative) {
        *value = INT64_MIN; *consumed = 3; return 1;
    }
    if (p[0] == '0' && p[1] == 'W' && p[2] == 'n') {
        *value = negative ? -INT64_MAX : INT64_MAX;
        *consumed = negative ? 4 : 3; return 1;
    }
    if (*p < '0' || *p > '9') return 0;
    uint64_t days = 0;
    unsigned day_digits = 0;
    while (*p >= '0' && *p <= '9') {
        if (++day_digits > 6) return 0;
        days = days * 10 + (unsigned)(*p++ - '0');
    }
    if (*p++ != 'D') return 0;
    unsigned h = 0, m = 0, seconds = 0;
    uint64_t fraction = 0;
    const char* field = p;
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9') { p++; if (++digits > 6) return 0; }
    if (digits) {
        if (*p == ':') {
            if (digits != 2) return 0;
            h = (unsigned)(field[0] - '0') * 10 + (unsigned)(field[1] - '0');
            p++;
            if (!two_digits(&p, &m)) return 0;
            if (*p == ':') { p++; if (!two_digits(&p, &seconds)) return 0; }
        } else {
            unsigned hour_digits = digits <= 2 ? digits : digits == 6 ? 2 : digits - 2;
            for (unsigned i = 0; i < hour_digits; i++) h = h * 10 + (unsigned)(field[i] - '0');
            if (digits > 2) m = (unsigned)(field[hour_digits] - '0') * 10 + (unsigned)(field[hour_digits + 1] - '0');
            if (digits == 6) seconds = (unsigned)(field[4] - '0') * 10 + (unsigned)(field[5] - '0');
        }
        if (m > 59 || seconds > 59) return 0;
        if (*p == '.') {
            p++;
            if (*p < '0' || *p > '9') return 0;
            unsigned n = 0;
            while (*p >= '0' && *p <= '9') {
                if (n < 9) { fraction = fraction * 10 + (unsigned)(*p - '0'); n++; }
                p++;
            }
            while (n++ < 9) fraction *= 10;
        }
    }
    uint64_t tail = ((uint64_t)h * 3600 + m * 60 + seconds) * UINT64_C(1000000000) + fraction;
    uint64_t ns = days * DAY_NS + tail;
    if (negative) ns = UINT64_C(0) - ns;
    if (ns == (UINT64_C(1) << 63)) return 0;
    memcpy(value, &ns, sizeof(ns));
    *consumed = (size_t)(p - text);
    return 1;
}

int ray_format_timespan(char* out, size_t size, int64_t value) {
    if (value == INT64_MIN) return snprintf(out, size, "0Nn");
    if (value == INT64_MAX) return snprintf(out, size, "0Wn");
    if (value == -INT64_MAX) return snprintf(out, size, "-0Wn");
    uint64_t ns = value < 0 ? (uint64_t)-value : (uint64_t)value;
    uint64_t days = ns / DAY_NS;
    ns %= DAY_NS;
    uint64_t seconds = ns / UINT64_C(1000000000);
    return snprintf(out, size, "%s%" PRIu64 "D%02u:%02u:%02u.%09u",
                    value < 0 ? "-" : "", days, (unsigned)(seconds / 3600),
                    (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60),
                    (unsigned)(ns % UINT64_C(1000000000)));
}
