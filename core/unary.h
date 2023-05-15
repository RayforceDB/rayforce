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

#ifndef MONAD_H
#define MONAD_H

#include "rayforce.h"

rf_object_t rf_til_i64(rf_object_t *x);
rf_object_t rf_distinct_i64(rf_object_t *x);
rf_object_t rf_sum_I64(rf_object_t *x);
rf_object_t rf_avg_I64(rf_object_t *x);
rf_object_t rf_min_I64(rf_object_t *x);
rf_object_t rf_max_I64(rf_object_t *x);
rf_object_t rf_sum_F64(rf_object_t *x);
rf_object_t rf_avg_F64(rf_object_t *x);
rf_object_t rf_min_F64(rf_object_t *x);
rf_object_t rf_max_F64(rf_object_t *x);
rf_object_t rf_count(rf_object_t *x);
rf_object_t rf_not_bool(rf_object_t *x);
rf_object_t rf_not_Bool(rf_object_t *x);

#endif
