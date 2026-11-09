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

#ifndef RAY_CAL_H
#define RAY_CAL_H

#include <stdint.h>

/* ===== Calendar primitives shared by format.c and parse.c ===== */

#define RAY_DATE_EPOCH 2000

/* Cumulative days-in-month lookup: [leap][month].
 * Index 0 = Jan start (0 days), index 12 = Dec end (365 or 366). */
static const uint32_t MONTHDAYS[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};

static inline int date_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

/* Days from 1970-01-01 to 2000-01-01 (RAY_DATE_EPOCH). */
#define RAY_DATE_EPOCH_UNIX_DAYS 10957

/* Decode: days-since-epoch → year/month/day.
 * Howard Hinnant's civil_from_days (floor-correct for negative years, so
 * year 0 and below agree with the year/month/day extractors in temporal.c). */
static inline void date_to_ymd(int32_t days, int* y, int* m, int* d) {
    int64_t z = (int64_t)days + RAY_DATE_EPOCH_UNIX_DAYS + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    int64_t yy = (int64_t)yoe + era * 400;
    int mo = (int)(mp < 10 ? mp + 3 : mp - 9);

    *y = (int)(yy + (mo <= 2));
    *m = mo;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
}

/* Encode: year/month/day → days-since-epoch (Hinnant days_from_civil). */
static inline int32_t ymd_to_date(int year, int month, int day) {
    int64_t y = (int64_t)year - (month <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t mp = month > 2 ? month - 3 : month + 9;
    if (mp < 0) mp = 0;  /* defensive: month is validated by callers */
    if (mp > 11) mp = 11;
    int64_t doy = (153 * mp + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int32_t)(era * 146097 + doe - 719468 - RAY_DATE_EPOCH_UNIX_DAYS);
}

#endif /* RAY_CAL_H */
