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

#ifndef CC_H
#define CC_H

#include "rayforce.h"
#include "debuginfo.h"

typedef enum cc_result_t
{
    CC_OK,
    CC_ERROR,
    CC_NULL,
} cc_result_t;

/*
 * Context for compiling lambda
 */
typedef struct cc_t
{
    bool_t top_level;       // is this top level lambda?
    rf_object *body;        // body of lambda being compiled (list of expressions)
    rf_object lambda;       // lambda being compiled
    debuginfo_t *debuginfo; // debuginfo from parse phase
} cc_t;

cc_result_t cc_compile_expr(bool_t has_consumer, cc_t *cc, rf_object object);
rf_object cc_compile_lambda(bool_t top, str_t name, rf_object args,
                            rf_object *body, u32_t id, i32_t len, debuginfo_t *debuginfo);
rf_object cc_compile(rf_object *body, debuginfo_t *debuginfo);

#endif
