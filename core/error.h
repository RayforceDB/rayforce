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

#ifndef ERROR_H
#define ERROR_H

#include "rayforce.h"
#include "nfo.h"
#include "heap.h"
#include "format.h"

#define as_error(obj) ((ray_error_t *)(as_string(obj)))

/*
 * Create a new error object and return it
 */
#define throw(t, ...)                       \
    {                                       \
        str_p _m = str_fmt(0, __VA_ARGS__); \
        obj_p _e = error_str(t, _m);        \
        heap_free(_m);                      \
        return _e;                          \
    }

#define panic(...)                                              \
    do                                                          \
    {                                                           \
        fprintf(stderr, "process panicked at: %s:%d in %s(): ", \
                __FILE__, __LINE__, __func__);                  \
        fprintf(stderr, __VA_ARGS__);                           \
        fprintf(stderr, "\n");                                  \
        exit(1);                                                \
    } while (0)

typedef struct loc_t
{
    span_t span;
    obj_p file;
    obj_p source;
} loc_t;

typedef struct ray_error_t
{
    i64_t code;
    obj_p msg;
    obj_p locs;
} ray_error_t;

obj_p error_obj(i8_t code, obj_p msg);
obj_p error_str(i8_t code, str_p msg);

#endif // ERROR_H
