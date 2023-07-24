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

#include <stdio.h>
#include "vary.h"
#include "heap.h"
#include "string.h"
#include "format.h"
#include "ops.h"
#include "util.h"

obj_t rf_call_vary_atomic(vary_t f, obj_t *x, i64_t n)
{
    i64_t i, lists = 0;

    for (i = 0; i < n; i++)
        if (is_vector(x[i]))
            lists++;

    if (lists == n)
        return f(x, n);
    else
        return f(x, n);
}

obj_t rf_call_vary(u8_t flags, vary_t f, obj_t *x, i64_t n)
{
    switch (flags)
    {
    case FLAG_ATOMIC:
        return rf_call_vary_atomic(f, x, n);
    default:
        return f(x, n);
    }
}

obj_trf_gc(obj_t *x, i64_t n)
{
    UNUSED(x);
    UNUSED(n);
    return i64(heap_gc());
}

obj_t rf_format(obj_t *x, i64_t n)
{
    str_t s = obj_t_fmt_n(x, n);
    obj_t ret;

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    ret = string_from_str(s, strlen(s));
    heap_free(s);

    return ret;
}

obj_t rf_print(obj_t *x, i64_t n)
{
    str_t s = obj_t_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s", s);
    heap_free(s);

    return null();
}

obj_t rf_println(obj_t *x, i64_t n)
{
    str_t s = obj_t_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s\n", s);
    heap_free(s);

    return null();
}