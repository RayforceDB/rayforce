/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limititation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT limitITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "format.h"
#include "rayforce.h"
#include "alloc.h"

#define MAX_I64_WIDTH 20
#define MAX_ROW_WIDTH MAX_I64_WIDTH * 2
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 4

const str_t PADDING = "                                                                                                   ";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern str_t value_fmt_ind(u32_t indent, u32_t limit, value_t *value);

extern str_t str_fmt(u32_t limit, str_t fmt, ...)
{
    i32_t n, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p = (str_t)rayforce_malloc(size);

    while (1)
    {
        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
        {
            rayforce_free(p);
            return "";
        }

        if (limit != 0 || n < size)
            break;

        size *= 2;
        p = rayforce_realloc(p, size);
    }

    return p;
}

str_t vector_fmt(u32_t limit, value_t *value)
{
    if (!limit)
        return "";

    if (value->list.len == 0)
        return str_fmt(3, "[]");

    str_t str, buf;
    i64_t count, len, remains = limit;
    i8_t v_type = value->type;

    str = buf = (str_t)rayforce_malloc(limit + FORMAT_TRAILER_SIZE);

    // Print '[' with the first element
    if (v_type == TYPE_I64)
        len = snprintf(buf, remains, "[%lld ", ((i64_t *)value->list.ptr)[0]);
    else if (v_type == TYPE_F64)
        len = snprintf(buf, remains, "[%.*f ", F64_PRECISION, ((f64_t *)value->list.ptr)[0]);
    else if (v_type == TYPE_SYMBOL)
        len = snprintf(buf, remains, "[%s ", symbols_get(((i64_t *)value->list.ptr)[0]));

    if (len < 0)
    {
        rayforce_free(str);
        return "";
    }

    buf += len;
    remains -= len;

    for (i64_t i = 1; i < value->list.len; i++)
    {
        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld ", ((i64_t *)value->list.ptr)[i]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f ", F64_PRECISION, ((f64_t *)value->list.ptr)[i]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s ", symbols_get(((i64_t *)value->list.ptr)[i]));

        if (len < 0)
        {
            rayforce_free(str);
            return "";
        }

        // printf("len: %lld, remains: %lld val: %lld\n", len, remains, ((i64_t *)value->list.ptr)[i]);

        if (len >= remains)
        {
            remains = 0;
            break;
        }
        else
        {
            remains -= len;
            buf += len;
        }
    }

    if (remains == 0)
        strncpy(buf, "..]", 4);
    else
        strncpy(buf - 1, "]", 2);

    return str;
}

str_t list_fmt(u32_t indent, u32_t limit, value_t *value)
{
    if (!limit)
        return "";

    if (value->list.ptr == NULL)
        return str_fmt(limit, "null");

    str_t s, str = str_fmt(limit, "(");

    indent += 2;

    for (u64_t i = 0; i < value->list.len; i++)
    {
        s = value_fmt_ind(indent, limit - indent, ((value_t *)value->list.ptr) + i);
        str = str_fmt(0, "%s\n%*.*s%s", str, indent, indent, PADDING, s);
    }

    str = str_fmt(0, "%s\n%*.*s)", str, indent - 2, indent - 2, PADDING);
    return str;
}

str_t error_fmt(u32_t indent, u32_t limit, value_t *value)
{
    return str_fmt(0, "** [E%.3d] error: %s", value->error.code, value->error.message);
}

str_t string_fmt(u32_t indent, u32_t limit, value_t *value)
{
    if (!limit)
        return "";

    if (value->list.ptr == NULL)
        return str_fmt(0, "\"\"");

    return str_fmt(value->list.len + 3, "\"%s\"", value->list.ptr);
}

str_t dict_fmt(u32_t indent, u32_t limit, value_t *value)
{
    if (!limit)
        return "";

    value_t *keys = &as_list(value)[0], *vals = &as_list(value)[1];
    u32_t len = keys->list.len;
    str_t k, v, str = str_fmt(limit, "{");

    indent += 2;

    for (u64_t i = 0; i < keys->list.len; i++)
    {
        k = value_fmt_ind(indent, limit - indent, ((value_t *)keys->list.ptr) + i);
        v = value_fmt_ind(indent, limit - indent, ((value_t *)vals->list.ptr) + i);
        str = str_fmt(0, "%s\n%*.*s%s: %s", str, indent, indent, PADDING, k, v);
    }

    str = str_fmt(0, "%s\n%*.*s}", str, indent - 2, indent - 2, PADDING);
    return str;
}

extern str_t value_fmt_ind(u32_t indent, u32_t limit, value_t *value)
{
    switch (value->type)
    {
    case TYPE_LIST:
        return list_fmt(indent, limit, value);
    case -TYPE_I64:
        return str_fmt(limit, "%lld", value->i64);
    case -TYPE_F64:
        return str_fmt(limit, "%.*f", F64_PRECISION, value->f64);
    case -TYPE_SYMBOL:
        return str_fmt(limit, "%s", symbols_get(value->i64));
    case TYPE_I64:
        return vector_fmt(limit, value);
    case TYPE_F64:
        return vector_fmt(limit, value);
    case TYPE_SYMBOL:
        return vector_fmt(limit, value);
    case TYPE_STRING:
        return string_fmt(indent, limit, value);
    case TYPE_DICT:
        return dict_fmt(indent, limit, value);
    case TYPE_ERROR:
        return error_fmt(indent, limit, value);
    default:
        return str_fmt(limit, "null");
    }
}

extern str_t value_fmt(value_t *value)
{
    u32_t indent = 0, limit = MAX_ROW_WIDTH - FORMAT_TRAILER_SIZE;
    return value_fmt_ind(indent, limit, value);
}
