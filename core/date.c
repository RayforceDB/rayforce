/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "date.h"
#include "temporal.h"
#include "util.h"
#include "ops.h"
#include "error.h"
#include "timestamp.h"
#include "parse.h"

RAY_ASSERT(sizeof(struct datestruct_t) == 16, "datestruct_t must be 16 bytes");

datestruct_t date_from_i32(i32_t offset) {
    u8_t leap;
    i32_t years, days, yy, mm, dd, mid;

    if (offset == NULL_I32)
        return (datestruct_t){.null = 1};

    offset += years_by_days(EPOCH - 1);
    years = (i32_t)ROUNDF64(((f64_t)offset / 365.2425));

    if (years_by_days(years) > offset)
        years -= 1;

    days = offset - years_by_days(years);
    yy = years + 1;
    leap = leap_year(yy);
    mid = 0;

    for (mid = 12; mid > 0; mid--)
        if (MONTHDAYS_FWD[leap][mid] != 0 && days / MONTHDAYS_FWD[leap][mid] != 0)
            break;

    if (mid == 12 || mid < 0)
        mid = 0;

    mm = (1 + mid % 12);
    dd = (1 + days - MONTHDAYS_FWD[leap][mid]);

    return (datestruct_t){
        .year = (u16_t)yy,
        .month = (u8_t)mm,
        .day = (u8_t)dd,
    };
}

datestruct_t date_from_str(str_p src, i64_t len) {
    i64_t cnt = 0, val = 0, digit_count, digit;
    datestruct_t dt = {.null = B8_FALSE, .year = 0, .month = 0, .day = 0};
    lit_p cur = src;

    if (!src || len == 0) {
        dt.null = B8_TRUE;
        return dt;
    }

    while (len > 0 && cnt < 3) {
        val = 0;
        digit_count = 0;

        while (len > 0 && is_digit(*cur)) {
            digit = *cur - '0';
            val = val * 10 + digit;
            cur++;
            len--;
            digit_count++;
        }

        if (digit_count == 0) {
            dt.null = B8_TRUE;
            return dt;
        }

        switch (cnt) {
            case 0:
                dt.year = (val >= 0) ? (u16_t)val : (dt.null = B8_TRUE, 0);
                break;
            case 1:
                dt.month = (val >= 1 && val <= 12) ? (u8_t)val : (dt.null = B8_TRUE, 0);
                break;
            case 2:
                dt.day = (val >= 1 && val <= 31) ? (u8_t)val : (dt.null = B8_TRUE, 0);
                break;
        }

        if (dt.null)
            return dt;

        cnt++;

        while (len > 0 && !is_digit(*cur)) {
            cur++;
            len--;
        }
    }

    if (cnt < 3)
        dt.null = B8_TRUE;

    return dt;
}

i32_t date_into_i32(datestruct_t dt) {
    i64_t leap, mdays, yy, mm, dd, ydays;

    yy = (dt.year > 0) ? dt.year - 1 : 0;
    ydays = years_by_days(yy);
    leap = leap_year(dt.year);
    mm = (dt.month > 0) ? dt.month - 1 : 0;
    mdays = MONTHDAYS_FWD[leap][mm];
    dd = dt.day;

    return ydays - years_by_days(EPOCH - 1) + mdays + dd - 1;
}

obj_p ray_date(obj_p arg) {
    timestamp_t ts;
    datestruct_t dt;

    if (arg->type != -TYPE_SYMBOL)
        return err_type(0, 0, 0, 0);

    ts = timestamp_current(str_from_symbol(arg->i64));
    dt.null = 0;
    dt.year = ts.year;
    dt.month = ts.month;
    dt.day = ts.day;

    return adate(date_into_i32(dt));
}
