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
#include "ops.h"

obj_t rf_call_binary(u8_t attrs, binary_f f, obj_t x, obj_t y);
obj_t rf_set(obj_t key, obj_t val);
obj_t rf_dict(obj_t x, obj_t y);
obj_t rf_table(obj_t x, obj_t y);
obj_t rf_rand(obj_t x, obj_t y);
obj_t rf_add(obj_t x, obj_t y);
obj_t rf_sub(obj_t x, obj_t y);
obj_t rf_mul(obj_t x, obj_t y);
obj_t rf_div(obj_t x, obj_t y);
obj_t rf_mod(obj_t x, obj_t y);
obj_t rf_fdiv(obj_t x, obj_t y);
obj_t rf_like(obj_t x, obj_t y);
obj_t rf_eq(obj_t x, obj_t y);
obj_t rf_ne(obj_t x, obj_t y);
obj_t rf_lt(obj_t x, obj_t y);
obj_t rf_gt(obj_t x, obj_t y);
obj_t rf_le(obj_t x, obj_t y);
obj_t rf_ge(obj_t x, obj_t y);
obj_t rf_and(obj_t x, obj_t y);
obj_t rf_or(obj_t x, obj_t y);
obj_t rf_and(obj_t x, obj_t y);
obj_t rf_at(obj_t x, obj_t y);
obj_t rf_find(obj_t x, obj_t y);
obj_t rf_concat(obj_t x, obj_t y);
obj_t rf_filter(obj_t x, obj_t y);
obj_t rf_take(obj_t x, obj_t y);
obj_t rf_in(obj_t x, obj_t y);
obj_t rf_sect(obj_t x, obj_t y);
obj_t rf_except(obj_t x, obj_t y);
obj_t rf_cast(obj_t x, obj_t y);
obj_t rf_group_Table(obj_t x, obj_t y);
obj_t rf_xasc(obj_t x, obj_t y);
obj_t rf_xdesc(obj_t x, obj_t y);

#endif
