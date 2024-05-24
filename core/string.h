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

#ifndef STRING_H
#define STRING_H

#include <string.h>
#include <stdarg.h>
#include "rayforce.h"

str_p str_chk_from_end(str_p pat);
b8_t str_starts_with(str_p str, str_p pat);
b8_t str_ends_with(str_p str, str_p pat);
b8_t str_match(str_p str, str_p pat);
obj_p string_from_str(lit_p str, u64_t len);
obj_p cstring_from_str(lit_p str, u64_t len);
obj_p cstring_from_obj(obj_p obj);
i64_t i64_from_str(lit_p str, u64_t len);
f64_t f64_from_str(lit_p str, u64_t len);
u64_t str_cpy(str_p dst, str_p src);
u64_t str_len(str_p s, u64_t n);
i64_t str_cmp(lit_p lhs, u64_t m, lit_p rhs, u64_t n);
obj_p vn_vstring(lit_p fmt, va_list args);

#endif // STRING_H
