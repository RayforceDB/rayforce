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

#ifndef IO_H
#define IO_H

#include "rayforce.h"

obj_p ray_hopen(obj_p *x, u64_t n);
obj_p ray_hclose(obj_p x);
obj_p ray_read(obj_p x);
obj_p ray_write(obj_p x, obj_p y);
obj_p ray_read_csv(obj_p *x, i64_t n);
obj_p ray_parse(obj_p x);
obj_p ray_eval(obj_p x);
obj_p ray_load(obj_p x);
obj_p ray_listen(obj_p x);
obj_p io_write(i64_t fd, u8_t msg_type, obj_p obj);
obj_p io_get_symfile(obj_p path);
obj_p io_set_table(obj_p path, obj_p table);
obj_p io_set_table_splayed(obj_p path, obj_p table, obj_p symfile);
obj_p io_get_table_splayed(obj_p path, obj_p symfile);

#endif  // IO_H
