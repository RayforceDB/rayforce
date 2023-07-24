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
#include "heap.h"
#include "ops.h"

i32_t u32_cmp(i64_t a, i64_t b)
{
    return !((u32_t)a == (u32_t)b);
}

debuginfo_t debuginfo_new(str_t filename, str_t lambda)
{
    debuginfo_t debuginfo = {
        .filename = filename,
        .lambda = lambda,
        .spans = ht_new(32, &rfi_kmh_hash, &u32_cmp),
    };

    return debuginfo;
}

nil_t debuginfo_insert(debuginfo_t *debuginfo, u32_t index, span_t span)
{
    u64_t s;
    memcpy(&s, &span, sizeof(span_t));
    ht_upsert(debuginfo->spans, (i64_t)index, (i64_t)s);
}

span_t debuginfo_get(debuginfo_t *debuginfo, u32_t index)
{
    i64_t s = ht_get(debuginfo->spans, (i64_t)index);

    if (s == NULL_I64)
        return (span_t){0};

    span_t span;
    memcpy(&span, &s, sizeof(span_t));

    return span;
}

nil_t debuginfo_free(debuginfo_t *debuginfo)
{
    ht_free(debuginfo->spans);
}