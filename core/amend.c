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

#include "amend.h"
#include "heap.h"
#include "util.h"
#include "runtime.h"

obj_t ray_amend(obj_t *x, u64_t n)
{
    u64_t i;
    obj_t obj, *env;

    if (n != 4)
        emit(ERR_LENGTH, "amend");

    switch (x[0]->type)
    {
    case -TYPE_SYMBOL:
        env = as_list(runtime_get()->env.variables);
        i = find_obj(env[0], x[0]);
        if (i == env[0]->len)
            emit(ERR_NOT_FOUND, "amend: object not found");
        obj = cow(as_list(env[1])[i]);
        break;
    default:
        obj = cow(x[0]);
        break;
    }

    obj = set_obj(&obj, x[1], clone(x[3]));
    return obj;
}

obj_t ray_dmend(obj_t *x, u64_t n)
{
    unused(x);
    unused(n);

    emit(ERR_NOT_IMPLEMENTED, "ray_dmend");
}