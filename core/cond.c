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

#include "cond.h"
#include "eval.h"
#include "ops.h"
#include "error.h"

obj_p ray_cond(obj_p *x, i64_t n) {
    obj_p res;

    switch (n) {
        case 2:
            res = eval(x[0]);
            if (IS_ERR(res))
                return res;
            if (ops_as_b8(res)) {
                drop_obj(res);
                return eval(x[1]);
            }
            drop_obj(res);
            return NULL_OBJ;
        case 3:
            res = eval(x[0]);
            if (IS_ERR(res))
                return res;
            if (ops_as_b8(res)) {
                drop_obj(res);
                return eval(x[1]);
            }
            drop_obj(res);
            return eval(x[2]);
        default:
            return ray_err("cond: invalid number of arguments");
    }
}
