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

#ifndef ITEMS_H
#define ITEMS_H

#include "rayforce.h"

obj_p ray_at(obj_p x, obj_p y);
obj_p ray_find(obj_p x, obj_p y);
obj_p ray_filter(obj_p x, obj_p y);
obj_p ray_take(obj_p x, obj_p y);
obj_p ray_in(obj_p x, obj_p y);
obj_p ray_sect(obj_p x, obj_p y);
obj_p ray_except(obj_p x, obj_p y);
obj_p ray_union(obj_p x, obj_p y);
obj_p ray_first(obj_p x);
obj_p ray_last(obj_p x);
obj_p ray_key(obj_p x);
obj_p ray_value(obj_p x);
obj_p ray_where(obj_p x);

#endif // ITEMS_H
