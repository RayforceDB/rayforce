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

static inline int32_t date_years_by_days(int yy) {
    return (int32_t)((int64_t)yy * 365 + yy / 4 - yy / 100 + yy / 400);
}

/* Decode: days-since-epoch → year/month/day */
static inline void date_to_ymd(int32_t days, int* y, int* m, int* d) {
    int32_t offset = days + date_years_by_days(RAY_DATE_EPOCH - 1);
    double approx = (double)offset / 365.2425;
    int32_t years = (int32_t)(approx >= 0.0 ? approx + 0.5 : approx - 0.5);

    if (date_years_by_days(years) > offset)
        years -= 1;

    int32_t rem = offset - date_years_by_days(years);
    int yy = years + 1;
    int leap = date_leap_year(yy);
    int mid = 0;

    for (mid = 12; mid > 0; mid--)
        if (MONTHDAYS[leap][mid] != 0 && rem / (int32_t)MONTHDAYS[leap][mid] != 0)
            break;

    if (mid == 12 || mid < 0)
        mid = 0;

    *y = yy;
    *m = 1 + mid % 12;
    *d = 1 + rem - (int32_t)MONTHDAYS[leap][mid];
}

/* Encode: year/month/day → days-since-epoch */
static inline int32_t ymd_to_date(int year, int month, int day) {
    int yy = (year > 0) ? year - 1 : 0;
    int32_t ydays = date_years_by_days(yy);
    int leap = date_leap_year(year);
    int mm = (month > 0) ? month - 1 : 0;
    int32_t mdays = (int32_t)MONTHDAYS[leap][mm];
    return ydays - date_years_by_days(RAY_DATE_EPOCH - 1) + mdays + day - 1;
}

#endif /* RAY_CAL_H */
