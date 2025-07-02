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

#ifndef MATH_H
#define MATH_H

#include "rayforce.h"

obj_p ray_add(obj_p x, obj_p y);
obj_p ray_sub(obj_p x, obj_p y);
obj_p ray_mul(obj_p x, obj_p y);
obj_p ray_div(obj_p x, obj_p y);
obj_p ray_mod(obj_p x, obj_p y);
obj_p ray_fdiv(obj_p x, obj_p y);
obj_p ray_xbar(obj_p x, obj_p y);
obj_p ray_round(obj_p x);
obj_p ray_floor(obj_p x);
obj_p ray_sum(obj_p x);
obj_p ray_cnt(obj_p x);
obj_p ray_avg(obj_p x);
obj_p ray_wavg(obj_p x, obj_p w);
obj_p ray_wavg_partial(obj_p x, obj_p w, i64_t len, i64_t offset, f64_t *weighted_sum, f64_t *weight_sum);
obj_p ray_min(obj_p x);
obj_p ray_max(obj_p x);
obj_p ray_ceil(obj_p x);
obj_p ray_med(obj_p x);
obj_p ray_dev(obj_p x);

#endif  // MATH_H
