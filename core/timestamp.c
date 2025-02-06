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
#include "date.h"
#include "timestamp.h"
#include "ops.h"
#include "util.h"
#include "error.h"
#include "string.h"
#include "parse.h"
#include "temporal.h"

RAYASSERT(sizeof(struct timestamp_t) == 16, timestamp_h)

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

i64_t date_into_days(datestruct_t dt) {
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

    datestruct_t dt = date_from_i32((i32_t)days);
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

    datestruct_t dt = {
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
    i64_t nanos;
    FILETIME ft;
    ULARGE_INTEGER uli;
    struct tm timeinfo_buf;
    struct tm *timeinfo;
    time_t rawtime;

    // Get system time in 100-nanosecond intervals since 1601-01-01
    GetSystemTimePreciseAsFileTime(&ft);

    // Convert to Unix epoch (1970-01-01)
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    // Windows FILETIME is in 100-nanosecond intervals, subtract Unix epoch offset
    const i64_t WINDOWS_TO_UNIX_EPOCH = 116444736000000000LL;
    uli.QuadPart -= WINDOWS_TO_UNIX_EPOCH;

    // Convert to seconds and nanoseconds
    rawtime = (time_t)(uli.QuadPart / 10000000LL);
    nanos = (uli.QuadPart % 10000000LL) * 100;  // Convert to nanoseconds

    // Determine if UTC or local time should be used
    if (tz && strcmp(tz, "utc") == 0) {
        gmtime_s(&timeinfo_buf, &rawtime);
    } else {
        localtime_s(&timeinfo_buf, &rawtime);
    }
    timeinfo = &timeinfo_buf;

    // Populate the timestamp_t struct
    timestamp_t ts = {.null = B8_FALSE,
                      .year = (u16_t)(timeinfo->tm_year + 1900),
                      .month = (u8_t)(timeinfo->tm_mon + 1),
                      .day = (u8_t)timeinfo->tm_mday,
                      .hours = (u8_t)timeinfo->tm_hour,
                      .mins = (u8_t)timeinfo->tm_min,
                      .secs = (u8_t)timeinfo->tm_sec,
                      .nanos = nanos};

    return ts;
}

#else

timestamp_t timestamp_current(lit_p tz) {
    struct timespec tspec;
    struct tm timeinfo_buf;
    struct tm *timeinfo;
    time_t rawtime;

    // Get current time with nanoseconds
    clock_gettime(CLOCK_REALTIME, &tspec);

    rawtime = tspec.tv_sec;

    // Choose UTC or local time
    if (tz != NULL && strcmp(tz, "utc") == 0)
        timeinfo = gmtime_r(&rawtime, &timeinfo_buf);
    else
        timeinfo = localtime_r(&rawtime, &timeinfo_buf);

    // Populate timestamp structure
    timestamp_t ts = {.null = B8_FALSE,
                      .year = (u16_t)(timeinfo->tm_year + 1900),
                      .month = (u8_t)(timeinfo->tm_mon + 1),
                      .day = (u8_t)timeinfo->tm_mday,
                      .hours = (u8_t)timeinfo->tm_hour,
                      .mins = (u8_t)timeinfo->tm_min,
                      .secs = (u8_t)timeinfo->tm_sec,
                      .nanos = tspec.tv_nsec};

    return ts;
}

#endif

obj_p ray_timestamp(obj_p arg) {
    if (arg->type != -TYPE_SYMBOL)
        THROW(ERR_TYPE, "timestamp: expected 'Symbol, got '%s'", type_name(arg->type));

    timestamp_t ts = timestamp_current(str_from_symbol(arg->i64));

    return timestamp(timestamp_into_i64(ts));
}