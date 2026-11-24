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

#ifndef RAY_CORE_TZ_H
#define RAY_CORE_TZ_H

/* ============================================================================
 * tz — a time zone as a table of UTC offsets
 *
 * ray_tz_parse reads one TZif file (RFC 8536, version 2 or later) from a
 * buffer and answers a table: the instants the zone's offset changes at, and
 * the offset on each side.  The rule a file ends with ("EST5EDT,M3.2.0,
 * M11.1.0") is unrolled into explicit changes to the end of the TIMESTAMP
 * range, so a lookup is one binary search and holds no rule logic.
 *
 * Instants are TIMESTAMP values: nanoseconds since 2000.01.01 UTC.
 * ========================================================================== */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ray_tz {
    int64_t* at;   /* n instants, ascending: the offset changes at at[i] */
    int64_t* off;  /* n + 1 offsets in ns, east positive: off[0] holds before
                    * at[0], off[i + 1] from at[i] on */
    int32_t  n;
} ray_tz_t;

/* Whether `name` is a zone name this module will look up: segments of
 * letters, digits, '_', '+' and '-' joined by '/'.  No '.', so a name can
 * never leave the directory it is resolved in. */
bool ray_tz_name_ok(const char* name, size_t len);

/* The table of the TZif file in buf[0..len), NULL when it is malformed, has
 * no 64-bit block, or counts leap seconds.  Free with ray_tz_free. */
ray_tz_t* ray_tz_parse(const uint8_t* buf, size_t len);
void      ray_tz_free(ray_tz_t* z);

/* The index into z->off of the offset in force at `ts`. */
static inline int32_t ray_tz_find(const ray_tz_t* z, int64_t ts) {
    int32_t lo = 0, hi = z->n;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        if (z->at[mid] <= ts) lo = mid + 1; else hi = mid;
    }
    return lo;
}

#endif /* RAY_CORE_TZ_H */
