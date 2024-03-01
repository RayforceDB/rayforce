/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef AGGR_H
#define AGGR_H

#include "rayforce.h"

obj_p aggr_sum(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_first(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_last(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_avg(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_max(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_min(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_count(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_med(obj_p val, obj_p bins, obj_p filter);
obj_p aggr_dev(obj_p val, obj_p bins, obj_p filter);

#endif // AGGR_H
