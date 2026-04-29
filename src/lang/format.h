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

#ifndef RAY_LANG_FORMAT_H
#define RAY_LANG_FORMAT_H

#include <rayforce.h>
#include <stdio.h>

#define FMT_TABLE_MAX_WIDTH   10
#define FMT_TABLE_MAX_HEIGHT  20
#define FMT_LIST_MAX_HEIGHT   50
#define FMT_DEFAULT_ROW_WIDTH 80
#define FMT_DEFAULT_PRECISION  2

/* Format a ray_t value into a new ray_t string (RAY_STR atom).
 * mode: 0 = compact, 1 = full (REPL), 2 = show (no limits) */
ray_t* ray_fmt(ray_t* obj, int mode);

/* Format and write to FILE* */
void ray_fmt_print(FILE* fp, ray_t* obj, int mode);

/* Display settings */
void ray_fmt_set_precision(int digits);
void ray_fmt_set_width(int cols);

/* Type name string (e.g. RAY_I64 -> "i64") */
const char* ray_type_name(int8_t type);

#endif /* RAY_LANG_FORMAT_H */
