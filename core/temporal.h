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

#ifndef TEMPORAL_H
#define TEMPORAL_H

#include "rayforce.h"

// An EPOCH starts from 2000.01.01T00:00:00.000
#define EPOCH 2000
#define UT_EPOCH_SHIFT 946684800ll

// secs between UT epoch and our
#define SECS_IN_DAY (i64_t)(24 * 60 * 60)
#define MSECS_IN_DAY (SECS_IN_DAY * 1000)
#define NSECS_IN_DAY (SECS_IN_DAY * 1000000000)

// nanos in a day and a millisecond
#define NANOS_FROM_DAY 86400000000000
#define NANOS_FROM_MILLIS 1000000

extern const u32_t MONTHDAYS_FWD[2][13];
extern const u32_t MONTHDAYS_ABS[2][12];

u8_t leap_year(u16_t year);
i32_t years_by_days(u16_t yy);
u8_t days_in_month(u16_t year, u8_t month);

#endif  // TEMPORAL_H
