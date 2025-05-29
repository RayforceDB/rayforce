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

#include <stdarg.h>
#include "error.h"
#include "heap.h"
#include "util.h"
#include "string.h"
#include "log.h"

obj_p error_obj(i8_t code, obj_p msg) {
    obj_p obj;
    ray_error_p e;

    LOG_DEBUG("Creating error object with code %d", code);

    obj = (obj_p)heap_alloc(sizeof(struct obj_t) + sizeof(struct ray_error_t));
    obj->mmod = MMOD_INTERNAL;
    obj->type = TYPE_ERR;
    obj->rc = 1;

    e = (ray_error_p)obj->raw;
    e->code = code;
    e->msg = msg;
    e->locs = NULL_OBJ;

    return obj;
}

obj_p error_str(i8_t code, lit_p msg) {
    LOG_DEBUG("Creating error string with code %d: %s", code, msg);
    return error_obj(code, cstring_from_str(msg, strlen(msg)));
}

obj_p error(i8_t code, lit_p fmt, ...) {
    obj_p e;
    va_list args;

    va_start(args, fmt);
    e = vn_vc8(fmt, args);
    va_end(args);

    LOG_ERROR("Error occurred: code=%d, message=%s", code, AS_C8(e));
    return error_obj(code, e);
}
