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

#ifndef COMPOSE_H
#define COMPOSE_H

#include "rayforce.h"

obj_p ray_dict(obj_p x, obj_p y);
obj_p ray_table(obj_p x, obj_p y);
obj_p ray_rand(obj_p x, obj_p y);
obj_p ray_cast_obj(obj_p x, obj_p y);
obj_p ray_enum(obj_p x, obj_p y);
obj_p ray_concat(obj_p x, obj_p y);
obj_p ray_distinct(obj_p x);
obj_p ray_til(obj_p x);
obj_p ray_reverse(obj_p x);
obj_p ray_group(obj_p x);
obj_p ray_guid(obj_p x);
obj_p ray_list(obj_p *x, u64_t n);
obj_p ray_enlist(obj_p *x, u64_t n);

#endif // COMPOSE_H
