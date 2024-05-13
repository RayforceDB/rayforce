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

#include "nfo.h"
#include "string.h"
#include "heap.h"
#include "ops.h"
#include "util.h"

obj_p nfo(obj_p filename, obj_p source)
{
    return vn_list(3, filename, source, ht_create(32, TYPE_I64));
}

nil_t nfo_insert(obj_p nfo, i64_t index, span_t span)
{
    i64_t i;

    if (nfo == NULL_OBJ)
        return;

    i = ht_tab_next(&as_list(nfo)[2], index);
    as_i64(as_list(as_list(nfo)[2])[0])[i] = index;
    memcpy(&as_i64(as_list(as_list(nfo)[2])[1])[i], &span, sizeof(span_t));
}

span_t nfo_get(obj_p nfo, i64_t index)
{
    i64_t i;
    span_t span = {0};

    if (!nfo)
        return span;

    i = ht_tab_next(&as_list(nfo)[2], index);
    if (as_i64(as_list(as_list(nfo)[2])[0])[i] == NULL_I64)
        return span;

    memcpy(&span, &as_i64(as_list(as_list(nfo)[2])[1])[i], sizeof(span_t));

    return span;
}
