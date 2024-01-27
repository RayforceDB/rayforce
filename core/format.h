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

#ifndef FORMAT_H
#define FORMAT_H

#include <stdarg.h>
#include "rayforce.h"

#define RED "\033[1;31m"
#define TOMATO "\033[1;38;5;9m"
#define GREEN "\033[1;32m"
#define YELLOW "\033[1;33m"
#define BLUE "\033[1;34m"
#define MAGENTA "\033[1;35m"
#define CYAN "\033[1;36m"
#define WHITE "\033[1;37m"
#define BLACK "\033[1;30m"
#define GRAY "\033[1;38;5;8m"
#define ORANGE "\033[1;38;5;202m"
#define PURPLE "\033[1;38;5;141m"
#define TEAL "\033[1;38;5;45m"
#define AQUA "\033[1;38;5;37m"
#define SALAD "\033[1;38;5;118m"
#define DARK_CYAN "\033[1;38;5;30m"
#define BOLD "\033[1m"
#define RESET "\033[0m"

extern i64_t str_vfmt_into(str_t *dst, i64_t *len, i64_t *offset, i64_t limit, str_t fmt, va_list vargs);
extern i64_t str_fmt_into(str_t *dst, i64_t *len, i64_t *offset, i64_t limit, str_t fmt, ...);
extern str_t str_fmt(i64_t limit, str_t fmt, ...);
extern str_t str_vfmt(i64_t limit, str_t fmt, va_list args);
extern obj_t obj_stringify(obj_t obj);
extern str_t obj_fmt(obj_t obj);
extern i64_t obj_fmt_into(str_t *dst, i64_t *len, i64_t *offset, i64_t indent, i64_t limit, bool_t full, obj_t obj);
extern str_t obj_fmt_n(obj_t *obj, u64_t n);

#endif
