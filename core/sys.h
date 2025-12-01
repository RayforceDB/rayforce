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

#ifndef SYS_H
#define SYS_H

#include "rayforce.h"

typedef struct sys_info_t {
    i32_t major_version;
    i32_t minor_version;
    c8_t build_date[256];
    c8_t git_hash[64];
    c8_t cpu[256];
    c8_t os[256];
    c8_t cwd[1024];
    i32_t mem;
    i32_t cores;
    i32_t threads;
} sys_info_t;

sys_info_t sys_info(i32_t threads);
obj_p sys_set_fpr(i32_t argc, str_p argv[]);
obj_p sys_use_unicode(i32_t argc, str_p argv[]);
obj_p sys_set_display_width(i32_t argc, str_p argv[]);
obj_p sys_timeit(i32_t argc, str_p argv[]);
obj_p sys_listen(i32_t argc, str_p argv[]);
obj_p sys_exit(i32_t argc, str_p argv[]);
obj_p ray_internal_command(obj_p cmd);
obj_p ray_system(obj_p cmd);
obj_p ray_sysinfo(obj_p *x, i64_t n);

#endif  // SYS_H
