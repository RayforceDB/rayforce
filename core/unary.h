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

typedef rf_object_t (*unary_t)(rf_object_t *);

rf_object_t rf_call_unary_atomic(unary_t f, rf_object_t *x);
rf_object_t rf_get_variable(rf_object_t *x);
rf_object_t rf_type(rf_object_t *x);
rf_object_t rf_count(rf_object_t *x);
rf_object_t rf_til(rf_object_t *x);
rf_object_t rf_distinct(rf_object_t *x);
rf_object_t rf_group(rf_object_t *x);
rf_object_t rf_sum(rf_object_t *x);
rf_object_t rf_avg(rf_object_t *x);
rf_object_t rf_min(rf_object_t *x);
rf_object_t rf_max(rf_object_t *x);
rf_object_t rf_not(rf_object_t *x);
rf_object_t rf_iasc(rf_object_t *x);
rf_object_t rf_idesc(rf_object_t *x);
rf_object_t rf_asc(rf_object_t *x);
rf_object_t rf_desc(rf_object_t *x);
rf_object_t rf_guid_generate(rf_object_t *x);
rf_object_t rf_neg(rf_object_t *x);
rf_object_t rf_where(rf_object_t *x);
rf_object_t rf_value(rf_object_t *x);

#endif
