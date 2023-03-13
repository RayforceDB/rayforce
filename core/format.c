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

typedef struct padding_t
{
    u32_t left;
    u32_t width;
    u32_t height;

} padding_t;

extern str_t str_fmt(u32_t lim, str_t fmt, ...)
{
    i32_t n, size = lim > 0 ? lim : MAX_ROW_WIDTH;
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
            return NULL;
        }

        if (lim != 0 || n < size)
            break;

        size *= 2;
        p = rayforce_realloc(p, size);
    }

    return p;
}

str_t vector_fmt(u32_t pad, u32_t lim, value_t *value)
{
    if (lim < FORMAT_TRAILER_SIZE)
        return NULL;

    str_t str, buf;
    i64_t count, remains, len;
    i8_t v_type = value->type;
    u8_t slim = lim - FORMAT_TRAILER_SIZE;

    remains = slim;
    str = buf = (str_t)rayforce_malloc(lim);
    len = snprintf(buf, remains, "%*.*s[", pad, pad, PADDING);

    if (len < 0)
    {
        rayforce_free(str);
        return NULL;
    }

    buf += len;

    count = value->list.len < 1 ? 0 : value->list.len - 1;
    len = 0;

    for (i64_t i = 0; i < count; i++)
    {
        remains = slim - (buf - str);

        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld ", ((i64_t *)value->list.ptr)[i]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f ", F64_PRECISION, ((f64_t *)value->list.ptr)[i]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s ", symbols_get(((i64_t *)value->list.ptr)[i]));

        if (len < 0)
        {
            rayforce_free(str);
            return NULL;
        }

        if (remains < FORMAT_TRAILER_SIZE)
        {
            buf = str + slim;
            break;
        }
        buf += len;
    }

    remains = slim - (buf - str);
    if (value->list.len > 0 && remains > FORMAT_TRAILER_SIZE)
    {
        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld", ((i64_t *)value->list.ptr)[count]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f", F64_PRECISION, ((f64_t *)value->list.ptr)[count]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s", symbols_get(((i64_t *)value->list.ptr)[count]));

        if (len < 0)
        {
            rayforce_free(str);
            return NULL;
        }
        if (remains < len)
            buf = str + slim;
        else
            buf += len;
    }

    if (remains < FORMAT_TRAILER_SIZE)
        strncpy(buf, "..]", 4);
    else
        strncpy(buf, "]", 2);

    return str;
}

str_t list_fmt(u32_t pad, u32_t lim, value_t *value)
{
    if (lim < FORMAT_TRAILER_SIZE)
        return NULL;

    if (value->list.ptr == NULL)
        return str_fmt(0, "null");

    str_t s, str, buf;
    u8_t slim = lim - FORMAT_TRAILER_SIZE;
    i64_t count, remains = slim, len;

    str = buf = (str_t)rayforce_malloc(2048);
    len = snprintf(buf, remains, "%*.*s(\n", pad, pad, PADDING);
    buf += len;

    if (len < 0)
    {
        rayforce_free(str);
        return NULL;
    }

    count = value->list.len < 1 ? 0 : value->list.len - 1;

    for (u64_t i = 0; i < value->list.len; i++)
    {
        s = value_fmt(((value_t *)value->list.ptr) + i);
        buf += snprintf(buf, MAX_ROW_WIDTH, "  %s\n", s);
    }

    strncpy(buf, ")", 2);
    return str;
}

str_t error_fmt(u32_t pad, u32_t lim, value_t *value)
{
    return str_fmt(0, "** [E%.3d] error: %s", value->error.code, value->error.message);
}

str_t string_fmt(u32_t pad, u32_t lim, value_t *value)
{
    if (value->list.ptr == NULL)
        return str_fmt(0, "\"\"");

    return str_fmt(value->list.len + 3, "\"%s\"", value->list.ptr);
}

str_t dict_fmt(u32_t pad, u32_t lim, value_t *value)
{
    value_t *keys = &as_list(value)[0], *vals = &as_list(value)[1];
    u32_t len = keys->list.len;

    if (lim < FORMAT_TRAILER_SIZE)
        return NULL;

    str_t s, str, buf;
    u8_t slim = lim - FORMAT_TRAILER_SIZE;
    i64_t count, remains = slim;

    str = buf = (str_t)rayforce_malloc(2048);
    len = snprintf(buf, remains, "%*.*s{\n", pad, pad, PADDING);
    buf += len;

    for (u64_t i = 0; i < len; i++)
    {
        s = value_fmt(((value_t *)value->list.ptr) + i);
        buf += snprintf(buf, MAX_ROW_WIDTH, "  %s\n", s);
    }

    return str;
}

extern str_t value_fmt(value_t *value)
{
    switch (value->type)
    {
    case TYPE_LIST:
        return list_fmt(0, MAX_ROW_WIDTH, value);
    case -TYPE_I64:
        return str_fmt(0, "%lld", value->i64);
    case -TYPE_F64:
        return str_fmt(0, "%.*f", F64_PRECISION, value->f64);
    case -TYPE_SYMBOL:
        return str_fmt(0, "%s", symbols_get(value->i64));
    case TYPE_I64:
        return vector_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_F64:
        return vector_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_SYMBOL:
        return vector_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_STRING:
        return string_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_DICT:
        return dict_fmt(0, MAX_ROW_WIDTH, value);
    case TYPE_ERROR:
        return error_fmt(0, 0, value);
    default:
        return str_fmt(0, "null");
    }
}
