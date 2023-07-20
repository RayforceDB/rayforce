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

typedef rf_object_t (*binary_t)(rf_object_t *, rf_object_t *);

rf_object_t rf_call_binary(u8_t flags, binary_t f, rf_object_t *x, rf_object_t *y);
rf_object_t rf_set_variable(rf_object_t *key, rf_object_t *val);
rf_object_t rf_dict(rf_object_t *x, rf_object_t *y);
rf_object_t rf_table(rf_object_t *x, rf_object_t *y);
rf_object_t rf_rand(rf_object_t *x, rf_object_t *y);
rf_object_t rf_add(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sub(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mul(rf_object_t *x, rf_object_t *y);
rf_object_t rf_div(rf_object_t *x, rf_object_t *y);
rf_object_t rf_mod(rf_object_t *x, rf_object_t *y);
rf_object_t rf_fdiv(rf_object_t *x, rf_object_t *y);
rf_object_t rf_like(rf_object_t *x, rf_object_t *y);
rf_object_t rf_eq(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ne(rf_object_t *x, rf_object_t *y);
rf_object_t rf_lt(rf_object_t *x, rf_object_t *y);
rf_object_t rf_gt(rf_object_t *x, rf_object_t *y);
rf_object_t rf_le(rf_object_t *x, rf_object_t *y);
rf_object_t rf_ge(rf_object_t *x, rf_object_t *y);
rf_object_t rf_and(rf_object_t *x, rf_object_t *y);
rf_object_t rf_or(rf_object_t *x, rf_object_t *y);
rf_object_t rf_and(rf_object_t *x, rf_object_t *y);
rf_object_t rf_get(rf_object_t *x, rf_object_t *y);
rf_object_t rf_find(rf_object_t *x, rf_object_t *y);
rf_object_t rf_concat(rf_object_t *x, rf_object_t *y);
rf_object_t rf_filter(rf_object_t *x, rf_object_t *y);
rf_object_t rf_take(rf_object_t *x, rf_object_t *y);
rf_object_t rf_in(rf_object_t *x, rf_object_t *y);
rf_object_t rf_sect(rf_object_t *x, rf_object_t *y);
rf_object_t rf_except(rf_object_t *x, rf_object_t *y);
rf_object_t rf_cast(rf_object_t *x, rf_object_t *y);
rf_object_t rf_group_Table(rf_object_t *x, rf_object_t *y);
rf_object_t rf_xasc(rf_object_t *x, rf_object_t *y);
rf_object_t rf_xdesc(rf_object_t *x, rf_object_t *y);

#endif
