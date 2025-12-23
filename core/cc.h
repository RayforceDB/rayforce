/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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
#include "lambda.h"

#define BC_SIZE 256

// Compiler context
typedef struct cc_ctx_t {
    i64_t ip;       // instruction pointer
    obj_p bc;       // bytecode buffer (U8 vector)
    obj_p args;     // arguments (SYMBOL vector)
    obj_p locals;   // local variables (LIST)
    obj_p consts;   // constants pool (LIST)
    i64_t lp;       // locals pointer
} cc_ctx_t;

// Compile lambda body to bytecode
// Takes a lambda object with args and body, fills in bc and consts
obj_p cc_compile(obj_p lambda);

// Dump bytecode for debugging
nil_t cc_dump(obj_p lambda);

#endif  // CC_H

