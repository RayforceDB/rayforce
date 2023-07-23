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

#ifndef BINARY_H
#define BINARY_H

#include "rayforce.h"

typedef rf_object (*binary_t)(rf_object, rf_object);

rf_object rf_call_binary(u8_t flags, binary_t f, rf_object x, rf_object y);
rf_object rf_set_variable(rf_object key, rf_object val);
rf_object rf_dict(rf_object x, rf_object y);
rf_object rf_table(rf_object x, rf_object y);
rf_object rf_rand(rf_object x, rf_object y);
rf_object rf_add(rf_object x, rf_object y);
rf_object rf_sub(rf_object x, rf_object y);
rf_object rf_mul(rf_object x, rf_object y);
rf_object rf_div(rf_object x, rf_object y);
rf_object rf_mod(rf_object x, rf_object y);
rf_object rf_fdiv(rf_object x, rf_object y);
rf_object rf_like(rf_object x, rf_object y);
rf_object rf_eq(rf_object x, rf_object y);
rf_object rf_ne(rf_object x, rf_object y);
rf_object rf_lt(rf_object x, rf_object y);
rf_object rf_gt(rf_object x, rf_object y);
rf_object rf_le(rf_object x, rf_object y);
rf_object rf_ge(rf_object x, rf_object y);
rf_object rf_and(rf_object x, rf_object y);
rf_object rf_or(rf_object x, rf_object y);
rf_object rf_and(rf_object x, rf_object y);
rf_object rf_get(rf_object x, rf_object y);
rf_object rf_find(rf_object x, rf_object y);
rf_object rf_concat(rf_object x, rf_object y);
rf_object rf_filter(rf_object x, rf_object y);
rf_object rf_take(rf_object x, rf_object y);
rf_object rf_in(rf_object x, rf_object y);
rf_object rf_sect(rf_object x, rf_object y);
rf_object rf_except(rf_object x, rf_object y);
rf_object rf_cast(rf_object x, rf_object y);
rf_object rf_group_Table(rf_object x, rf_object y);
rf_object rf_xasc(rf_object x, rf_object y);
rf_object rf_xdesc(rf_object x, rf_object y);

#endif
