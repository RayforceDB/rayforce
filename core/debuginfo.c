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

#include "debuginfo.h"
#include "string.h"

i64_t u32_hash(null_t *val)
{
    return (i64_t)val;
}

i32_t u32_cmp(null_t *a, null_t *b)
{
    return !((u32_t)(i64_t)a == (u32_t)(i64_t)b);
}

debuginfo_t debuginfo_new(str_t filename, str_t function)
{
    debuginfo_t debuginfo = {
        .filename = filename,
        .function = function,
        .spans = ht_new(&u32_hash, &u32_cmp),
    };

    return debuginfo;
}

null_t debuginfo_insert(debuginfo_t *debuginfo, u32_t index, span_t span)
{
    u64_t s;
    memcpy(&s, &span, sizeof(span_t));
    ht_update(debuginfo->spans, (null_t *)(i64_t)index, (null_t *)s);
}

span_t debuginfo_get(debuginfo_t *debuginfo, u32_t index)
{
    u64_t *s = (u64_t *)ht_get(debuginfo->spans, (null_t *)(i64_t)index);

    if (!s)
        return (span_t){0};

    span_t span;
    memcpy(&span, &s, sizeof(span_t));

    return span;
}

null_t debuginfo_free(debuginfo_t *debuginfo)
{
    ht_free(debuginfo->spans);
}