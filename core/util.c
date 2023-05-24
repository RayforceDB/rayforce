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

#include <stdlib.h>
#include "util.h"
#include "format.h"

u32_t next_power_of_two_u32(u32_t n)
{
    if (n == 0)
        return 1;
    // If n is already a power of 2
    if ((n & (n - 1)) == 0)
        return n;

    return 1 << (32 - __builtin_clz(n));
}

u64_t next_power_of_two_u64(u64_t n)
{
    if (n == 0)
        return 1;
    // If n is already a power of 2
    if ((n & (n - 1)) == 0)
        return n;

    return 1UL << (64 - __builtin_clzl(n));
}

i64_t size_of(i8_t type)
{
    switch (type)
    {
    case TYPE_BOOL:
        return 1;
    case TYPE_I64:
        return 8;
    case TYPE_F64:
        return 8;
    case TYPE_CHAR:
        return 1;
    case TYPE_LIST:
        return sizeof(struct rf_object_t);
    default:
        panic(str_fmt(0, "Unknown type: %d", type));
    }
}