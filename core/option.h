/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef OPTION_H
#define OPTION_H

#include "rayforce.h"
#include "util.h"
#include <assert.h>

typedef enum {
    OPTION_CODE_NONE = 0ll,
    OPTION_CODE_SOME = 1ll,
    OPTION_CODE_ERROR = -1ll,
} option_code_t;

// Pack the struct to avoid padding and ensure consistent size
typedef struct __attribute__((aligned(16))) {
    option_code_t code;
    raw_p value;
} option_t;

RAYASSERT(sizeof(option_t) == 16, option_h)

// Create a Some variant with a value
static inline __attribute__((always_inline, const)) option_t option_some(raw_p val) {
    return (option_t){.code = OPTION_CODE_SOME, .value = val};
}

// Create a None variant
static inline __attribute__((always_inline, const)) option_t option_none() {
    return (option_t){.code = OPTION_CODE_NONE, .value = NULL};
}

// Create an Error variant
static inline __attribute__((always_inline, const)) option_t option_error(obj_p msg) {
    return (option_t){.code = OPTION_CODE_ERROR, .value = (raw_p)msg};
}

// Check if the option contains a value
static inline __attribute__((always_inline, pure)) b8_t option_is_some(const option_t* opt) {
    return opt->code == OPTION_CODE_SOME;
}

// Check if the option is None
static inline __attribute__((always_inline, pure)) b8_t option_is_none(const option_t* opt) {
    return opt->code == OPTION_CODE_NONE;
}

// Check if the option is an error
static inline __attribute__((always_inline, pure)) b8_t option_is_error(const option_t* opt) {
    return opt->code == OPTION_CODE_ERROR;
}

// Get the value from a Some variant (with assertion)
static inline __attribute__((always_inline)) raw_p option_unwrap(const option_t* opt) {
    assert(option_is_some(opt));
    return opt->value;
}

// Get the value from a Some or Error variant without assertion
static inline __attribute__((always_inline)) raw_p option_take(const option_t* opt) { return opt->value; }

// Get the value from a Some variant or return a default value
static inline __attribute__((always_inline)) raw_p option_unwrap_or(const option_t* opt, raw_p default_val) {
    return option_is_some(opt) ? opt->value : default_val;
}

// Map a function over the value if it exists
static inline __attribute__((always_inline)) option_t option_map(const option_t* opt, raw_p (*f)(raw_p)) {
    if (option_is_some(opt)) {
        return option_some(f(opt->value));
    }
    return *opt;
}

// Chain operations on the value if it exists
static inline __attribute__((always_inline)) option_t option_and_then(const option_t* opt, option_t (*f)(raw_p)) {
    if (option_is_some(opt)) {
        return f(opt->value);
    }
    return *opt;
}

#endif  // OPTION_H
