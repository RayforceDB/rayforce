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

#include "misc.h"
#include "ops.h"
#include "util.h"
#include "runtime.h"
#include "sock.h"
#include "fs.h"
#include "items.h"
#include "unary.h"
#include "error.h"
#include "index.h"

obj_p ray_type(obj_p x)
{
    i8_t type;
    if (!x)
        type = -TYPE_ERROR;
    else
        type = x->type;

    i64_t t = env_get_typename_by_type(&runtime_get()->env, type);
    return symboli64(t);
}

obj_p ray_count(obj_p x)
{
    return i64(ops_count(x));
}

obj_p ray_rc(obj_p x)
{
    // substract 1 to skip the our reference
    return i64(rc_obj(x) - 1);
}

obj_p ray_quote(obj_p x)
{
    return clone_obj(x);
}