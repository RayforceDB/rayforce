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

#include <stddef.h>
#include <stdio.h>
#include "string.h"
#include "alloc.h"
#include "nary.h"
#include "format.h"

rf_object_t rf_list(rf_object_t *x, u32_t n)
{
    rf_object_t l = list(n);
    u32_t i;

    for (i = 0; i < n; i++)
    {
        rf_object_t *item = x + i;
        as_list(&l)[i] = rf_object_clone(item);
    }

    return l;
}

rf_object_t rf_format(rf_object_t *x, u32_t n)
{
    str_t s = rf_object_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    return string_from_str(s, strlen(s));
}

rf_object_t rf_print(rf_object_t *x, u32_t n)
{
    str_t s = rf_object_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s", s);
    rf_free(s);

    return null();
}

rf_object_t rf_println(rf_object_t *x, u32_t n)
{
    str_t s = rf_object_fmt_n(x, n);

    if (!s)
        return error(ERR_TYPE, "malformed format string");

    printf("%s\n", s);
    rf_free(s);

    return null();
}