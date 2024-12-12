/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#include <errno.h>
#include <limits.h>
#include "timestamp.h"
#include "ops.h"
#include "util.h"
#include "error.h"
#include "string.h"
#include "parse.h"

CASSERT(sizeof(struct timestamp_t) == 16, timestamp_h)

// An EPOCH starts from 2000.01.01T00:00:00.000
#define EPOCH 2000
#define UT_EPOCH_SHIFT 946684800ll
// secs between UT epoch and our
#define SECS_IN_DAY (i64_t)(24 * 60 * 60)
#define MSECS_IN_DAY (SECS_IN_DAY * 1000)
#define NSECS_IN_DAY (SECS_IN_DAY * 1000000000)

const u32_t MONTHDAYS_FWD[2][13] = {
    {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365},
    {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366},
};
const u32_t MONTHDAYS_ABS[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

typedef struct date_t {
    u16_t year;
    u8_t month;
    u8_t day;
} date_t;

typedef struct timespan_t {
    u8_t hours;
    u8_t mins;
    u8_t secs;
    u32_t nanos;
} timespan_t;

u8_t leap_year(u16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

i64_t years_by_days(u16_t yy) { return (yy * 365 + yy / 4 - yy / 100 + yy / 400); }

u8_t days_in_month(u16_t year, u8_t month) {
    u8_t leap = leap_year(year);
    return (MONTHDAYS_ABS[leap][month > 0 ? month - 1 : 0]);
}

date_t date_from_days(i64_t v) {
    v += years_by_days(EPOCH - 1);
    i64_t years = ROUNDF64(((f64_t)v / 365.2425));

    if (years_by_days(years) > v)
        years -= 1;

    i64_t days = v - years_by_days(years);
    i64_t yy = years + 1;
    u8_t leap = leap_year(yy);
    i32_t mid = 0;

    for (mid = 12; mid > 0; mid--)
        if (MONTHDAYS_FWD[leap][mid] != 0 && days / MONTHDAYS_FWD[leap][mid] != 0)
            break;

    if (mid == 12 || mid < 0)
        mid = 0;

    i64_t mm = (1 + mid % 12);
    i64_t dd = (1 + days - MONTHDAYS_FWD[leap][mid]);

    return (date_t){
        .year = (u16_t)yy,
        .month = (u8_t)mm,
        .day = (u8_t)dd,
    };
}

timespan_t timespan_from_nanos(i64_t nanos) {
    i64_t secs = nanos / 1000000000;
    i64_t ns = nanos % 1000000000;
    i64_t hh = secs / 3600;
    i64_t mins = secs % 3600;
    i64_t mm = mins / 60;
    i64_t ss = mins % 60;

    return (timespan_t){
        .hours = (u8_t)hh,
        .mins = (u8_t)mm,
        .secs = (u8_t)ss,
        .nanos = (u32_t)ns,
    };
}

i64_t timespan_into_nanos(timespan_t ts) {
    i64_t hrs = (i64_t)ts.hours * 3600;
    i64_t mns = (i64_t)ts.mins * 60;
    return ((hrs + mns + (i64_t)ts.secs) * 1000000000 + ts.nanos);
}

i64_t date_into_days(date_t dt) {
    i64_t ydays = years_by_days(dt.year > 0 ? dt.year - 1 : dt.year);
    u8_t leap = leap_year(dt.year);
    u32_t mdays = MONTHDAYS_FWD[leap][dt.month > 0 ? dt.month - 1 : 0];
    return (ydays - years_by_days(EPOCH - 1) + mdays + dt.day - 1);
}

//
timestamp_t timestamp_from_i64(i64_t offset) {
    i64_t days = offset / NSECS_IN_DAY;
    i64_t span = offset % NSECS_IN_DAY;

    if (span < 0) {
        days -= 1;
        span += NSECS_IN_DAY;
    }

    date_t dt = date_from_days(days);
    timespan_t sp = timespan_from_nanos(span);

    timestamp_t ts = {
        .null = B8_FALSE,
        .year = dt.year,
        .month = dt.month,
        .day = dt.day,
        .hours = sp.hours,
        .mins = sp.mins,
        .secs = sp.secs,
        .nanos = sp.nanos,
    };

    return ts;
}

i64_t timestamp_into_i64(timestamp_t ts) {
    if (ts.null)
        return NULL_I64;

    date_t dt = {
        .year = ts.year,
        .month = ts.month,
        .day = ts.day,
    };

    timespan_t sp = {
        .hours = ts.hours,
        .mins = ts.mins,
        .secs = ts.secs,
        .nanos = ts.nanos,
    };

    i64_t dss = date_into_days(dt);
    i64_t offs = timespan_into_nanos(sp);

    if (dss < 0) {
        offs = NSECS_IN_DAY - offs;
        return ((dss + 1) * NSECS_IN_DAY - offs);
    }

    return (dss * NSECS_IN_DAY + offs);
}

timestamp_t timestamp_from_str(str_p src, u64_t len) {
    i64_t cnt = 0, val = 0, digit_count, digit;
    timestamp_t ts = {.null = B8_FALSE, .year = 0, .month = 0, .day = 0, .hours = 0, .mins = 0, .secs = 0, .nanos = 0};
    lit_p cur = src;

    if (!src || len == 0) {
        ts.null = B8_TRUE;
        return ts;
    }

    while (len > 0 && cnt < 7) {
        // Reset value for the next number
        val = 0;
        digit_count = 0;

        // Parse the next numeric value
        while (len > 0 && is_digit(*cur)) {
            digit = *cur - '0';

            // Check for overflow before adding the next digit
            if (val > (LONG_MAX - digit) / 10) {
                ts.null = B8_TRUE;
                return ts;
            }

            val = val * 10 + digit;
            cur++;
            len--;
            digit_count++;
        }

        if (digit_count == 0) {  // No digits found
            ts.null = B8_TRUE;
            return ts;
        }

        // Validate and assign values
        switch (cnt) {
            case 0:
                ts.year = (val >= 0) ? (u16_t)val : (ts.null = B8_TRUE, 0);
                break;
            case 1:
                ts.month = (val >= 1 && val <= 12) ? (u8_t)val : (ts.null = B8_TRUE, 0);
                break;
            case 2:
                ts.day = (val >= 1 && val <= 31) ? (u8_t)val : (ts.null = B8_TRUE, 0);
                break;
            case 3:
                ts.hours = (val >= 0 && val <= 23) ? (u8_t)val : (ts.null = B8_TRUE, 0);
                break;
            case 4:
                ts.mins = (val >= 0 && val <= 59) ? (u8_t)val : (ts.null = B8_TRUE, 0);
                break;
            case 5:
                ts.secs = (val >= 0 && val <= 59) ? (u8_t)val : (ts.null = B8_TRUE, 0);
                break;
            case 6:
                ts.nanos = (u32_t)val;
                break;
        }

        if (ts.null)
            return ts;

        cnt++;

        // Skip non-digit characters
        while (len > 0 && !is_digit(*cur)) {
            cur++;
            len--;
        }
    }

    if (cnt < 3)
        ts.null = B8_TRUE;

    return ts;
}

#if defined(OS_WINDOWS)

timestamp_t timestamp_current(lit_p tz) {
    time_t rawtime;
    struct tm *timeinfo;

    // Get the current time
    time(&rawtime);

    // Determine if UTC or local time should be used based on the argument
    if (tz != NULL && strcmp(tz, "utc") == 0)
        timeinfo = gmtime(&rawtime);  // UTC time
    else
        timeinfo = localtime(&rawtime);  // Local time

    // Populate the timestamp_t struct
    timestamp_t ts = {
        .null = B8_FALSE,
        .year = (u16_t)(timeinfo->tm_year + 1900),
        .month = (u8_t)(timeinfo->tm_mon + 1),
        .day = (u8_t)timeinfo->tm_mday,
        .hours = (u8_t)timeinfo->tm_hour,
        .mins = (u8_t)timeinfo->tm_min,
        .secs = (u8_t)timeinfo->tm_sec,
        .nanos = 0  // Windows does not support nanoseconds in time_t
    };

    return ts;
}

#else

timestamp_t timestamp_current(lit_p tz) {
    time_t rawtime;
    struct tm *timeinfo;

    // Get the current time
    time(&rawtime);

    // Choose local time or UTC based on the argument
    if (tz != NULL && strcmp(tz, "utc") == 0)
        timeinfo = gmtime(&rawtime);  // UTC time
    else
        timeinfo = localtime(&rawtime);  // Local time

    timestamp_t ts = {
        .null = B8_FALSE,
        .year = (u16_t)(timeinfo->tm_year + 1900),
        .month = (u8_t)(timeinfo->tm_mon + 1),
        .day = (u8_t)timeinfo->tm_mday,
        .hours = (u8_t)timeinfo->tm_hour,
        .mins = (u8_t)timeinfo->tm_min,
        .secs = (u8_t)timeinfo->tm_sec,
        .nanos = 0,
    };

    return ts;
}

#endif

obj_p ray_timestamp(obj_p arg) {
    if (arg->type != -TYPE_SYMBOL)
        THROW(ERR_TYPE, "timestamp: expected 'Symbol, got '%s'", type_name(arg->type));

    timestamp_t ts = timestamp_current(str_from_symbol(arg->i64));

    return timestamp(timestamp_into_i64(ts));
}