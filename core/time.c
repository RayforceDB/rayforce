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

#include "time.h"
#include "util.h"
#include "error.h"
#include "timestamp.h"

RAYASSERT(sizeof(struct timestruct_t) == 16, time_h)

timestruct_t time_from_i32(i32_t offset) {
    b8_t sign;
    u8_t hh, mm, ss;
    u16_t ms;
    i32_t mask;
    i64_t val, min, secs;

    if (offset == NULL_I32)
        return (timestruct_t){.null = 1};

    mask = offset >> 31;
    val = (mask ^ offset) - mask;
    sign = (mask != 0) ? -1 : 1;

    secs = val / 1000;
    ms = (val % 1000);
    hh = (secs / 3600);
    min = secs % 3600;
    mm = (min / 60);
    ss = (min % 60);

    return (timestruct_t){.null = 0, .sign = sign, .hours = hh, .mins = mm, .secs = ss, .msecs = ms};
}

timestruct_t time_from_str(str_p src, i64_t len) {
    i64_t i;
    i64_t cnt = 0, val = 0, digit;
    timestruct_t ts = {.null = 0, .sign = 1, .hours = 0, .mins = 0, .secs = 0, .msecs = 0};

    for (i = 0; i < len; i++) {
        if (src[i] == '-') {
            ts.sign = -1;
            continue;
        }

        if (src[i] == ':' || src[i] == '.') {
            switch (cnt) {
                case 0:
                    ts.hours = (u8_t)val;
                    break;
                case 1:
                    ts.mins = (u8_t)val;
                    break;
                case 2:
                    ts.secs = (u8_t)val;
                    break;
                default:
                    return (timestruct_t){.null = 1};
            }

            cnt++;
            val = 0;
            continue;
        }

        if (src[i] < '0' || src[i] > '9') {
            return (timestruct_t){.null = 1};
        }

        digit = src[i] - '0';
        val = val * 10 + digit;
    }

    switch (cnt) {
        case 2:
            ts.secs = (u8_t)val;
            break;
        case 3:
            ts.msecs = (u16_t)val;
            break;
        default:
            ts.null = 1;
            break;
    }

    return ts;
}

i32_t time_into_i32(timestruct_t tm) {
    i64_t hh, mm, ss, ms;
    i32_t sign;

    if (tm.null)
        return NULL_I32;

    sign = tm.sign;

    hh = tm.hours;
    mm = tm.mins;
    ss = tm.secs;
    ms = tm.msecs;

    return sign * (i32_t)((hh * 3600 + mm * 60 + ss) * 1000 + ms);
}

obj_p ray_time(obj_p arg) {
    timestamp_t ts;
    timestruct_t tm;

    if (arg->type != -TYPE_SYMBOL)
        return ray_err(ERR_TYPE);

    ts = timestamp_current(str_from_symbol(arg->i64));
    tm.sign = 1;
    tm.null = 0;
    tm.hours = ts.hours;
    tm.mins = ts.mins;
    tm.secs = ts.secs;
    tm.msecs = ts.nanos / 1000000;

    return atime(time_into_i32(tm));
}
