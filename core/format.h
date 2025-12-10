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
#define LIGHT_BLUE "\033[1;38;5;39m"
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
#define BACK_RED "\033[41m"
#define BACK_GREEN "\033[42m"
#define BACK_YELLOW "\033[43m"
#define BACK_BLUE "\033[44m"
#define BACK_MAGENTA "\033[45m"
#define BACK_CYAN "\033[46m"
#define BACK_WHITE "\033[47m"
#define RESET "\033[0m"

#define LIT_NULL_C8 ""
#define LIT_NULL_I16 "0Nh"
#define LIT_NULL_I32 "0Ni"
#define LIT_NULL_I64 "0Nl"
#define LIT_NULL_F64 "0Nf"
#define LIT_NULL_DATE "0Nd"
#define LIT_NULL_TIME "0Nt"
#define LIT_NULL_TIMESTAMP "0Np"
#define LIT_NULL_GUID "0Ng"
#define LIT_NULL_SYMBOL "0Ns"
#define LIT_NULL "0N0"

i64_t str_vfmt_into(obj_p *dst, i64_t limit, lit_p fmt, va_list vargs);
i64_t str_fmt_into(obj_p *dst, i64_t limit, lit_p fmt, ...);
obj_p str_fmt(i64_t limit, lit_p fmt, ...);
obj_p str_vfmt(i64_t limit, lit_p fmt, va_list args);
obj_p obj_stringify(obj_p obj);
i64_t prompt_fmt_into(obj_p *dst);
i64_t continuation_prompt_fmt_into(obj_p *dst);
// full: 0 = compact, 1 = full with limits, 2 = full without limits (show)
obj_p obj_fmt(obj_p obj, i64_t full);
i64_t obj_fmt_into(obj_p *dst, i64_t indent, i64_t limit, i64_t full, obj_p obj);
i64_t guid_fmt_into(obj_p *dst, guid_t *val);
obj_p obj_fmt_n(obj_p *obj, i64_t n);
obj_p ray_show(obj_p obj);
obj_p timeit_fmt(nil_t);
i64_t format_set_use_unicode(b8_t use);   // set use unicode
i64_t format_get_use_unicode();           // get use unicode
i64_t format_set_fpr(i64_t x);            // set float precision
i64_t format_set_display_width(i64_t x);  // set display width

#endif  // FORMAT_H
