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

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "format.h"
#include "rayforce.h"
#include "alloc.h"
#include "util.h"
#include "dict.h"

#define MAX_I64_WIDTH 20
#define MAX_ROW_WIDTH MAX_I64_WIDTH * 2
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 4
#define TABLE_MAX_WIDTH 10  // Maximum number of columns
#define TABLE_MAX_HEIGHT 10 // Maximum number of rows

const str_t PADDING = "                                                                                                   ";
const str_t TABLE_SEPARATOR = " | ";
const str_t TABLE_HEADER_SEPARATOR = "------------------------------------------------------------------------------------";

extern str_t value_fmt_ind(u32_t indent, u32_t limit, value_t *value);

extern i32_t str_fmt_into(u32_t limit, i32_t offset, str_t *dst, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p;

    n = strlen(*dst);

    if (n < (size + offset))
    {
        size = size + offset;
        *dst = rayforce_realloc(*dst, size);
    }

    while (1)
    {
        p = *dst + offset;

        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
            return n;

        if (limit != 0 || n < size)
            break;

        size = size * 2;
        *dst = rayforce_realloc(*dst, size);
    }

    return n;
}

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
    i64_t len = 0, remains = limit;
    i8_t v_type = value->type;

    str = buf = (str_t)rayforce_malloc(limit + FORMAT_TRAILER_SIZE);

    // Print '[' with the first element
    if (v_type == TYPE_I64)
        len = snprintf(buf, remains, "[%lld ", as_vector_i64(value)[0]);
    else if (v_type == TYPE_F64)
        len = snprintf(buf, remains, "[%.*f ", F64_PRECISION, as_vector_f64(value)[0]);
    else if (v_type == TYPE_SYMBOL)
        len = snprintf(buf, remains, "[%s ", symbols_get(as_vector_symbol(value)[0]));

    if (len < 0)
    {
        rayforce_free(str);
        return "";
    }

    buf += len;
    remains -= len;

    for (u64_t i = 1; i < value->list.len; i++)
    {
        if (v_type == TYPE_I64)
            len = snprintf(buf, remains, "%lld ", as_vector_i64(value)[i]);
        else if (v_type == TYPE_F64)
            len = snprintf(buf, remains, "%.*f ", F64_PRECISION, as_vector_f64(value)[i]);
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s ", symbols_get(as_vector_symbol(value)[i]));

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
    i32_t offset = 1;

    indent += 2;

    for (u64_t i = 0; i < value->list.len; i++)
    {
        s = value_fmt_ind(indent, limit - indent, ((value_t *)value->list.ptr) + i);
        offset += str_fmt_into(0, offset, &str, "\n%*.*s%s", indent, indent, PADDING, s);
        rayforce_free(s);
    }

    str_fmt_into(0, offset, &str, "\n%*.*s)", indent - 2, indent - 2, PADDING);
    return str;
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
    str_t k, v, str = str_fmt(limit, "{");
    u32_t offset = 1;

    indent += 2;

    for (u64_t i = 0; i < keys->list.len; i++)
    {
        // Dispatch keys type
        switch (keys->type)
        {
        case TYPE_I64:
            k = str_fmt(limit, "%lld", as_vector_i64(keys)[i]);
            break;
        case TYPE_F64:
            k = str_fmt(limit, "%.*f", F64_PRECISION, as_vector_f64(keys)[i]);
            break;
        case TYPE_SYMBOL:
            k = str_fmt(limit, "%s", symbols_get(as_vector_symbol(keys)[i]));
            break;
        default:
            k = value_fmt_ind(indent, limit - indent, &as_list(keys)[i]);
            break;
        }
        // Dispatch values type
        switch (vals->type)
        {
        case TYPE_I64:
            v = str_fmt(limit, "%lld", as_vector_i64(vals)[i]);
            break;
        case TYPE_F64:
            v = str_fmt(limit, "%.*f", F64_PRECISION, as_vector_f64(vals)[i]);
            break;
        case TYPE_SYMBOL:
            v = str_fmt(limit, "%s", symbols_get(as_vector_symbol(vals)[i]));
            break;
        default:
            v = value_fmt_ind(indent, limit - indent, &as_list(vals)[i]);
            break;
        }

        offset += str_fmt_into(0, offset, &str, "\n%*.*s%s: %s", indent, indent, PADDING, k, v);
        rayforce_free(k);
        rayforce_free(v);
    }

    str_fmt_into(0, offset, &str, "\n%*.*s}", indent - 2, indent - 2, PADDING);
    return str;
}

str_t table_fmt(u32_t indent, u32_t limit, value_t *value)
{
    if (!limit)
        return "";

    i64_t *header = as_vector_symbol(&as_list(value)[0]);
    value_t *columns = &as_list(value)[1], column_widths;
    u32_t table_width, width, table_height;
    str_t str = str_fmt(0, "|"), s;
    str_t formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL}};
    i32_t offset = 1;

    table_width = (&as_list(value)[0])->list.len;
    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    table_height = (&as_list(columns)[0])->list.len;
    if (table_height > TABLE_MAX_HEIGHT)
        table_height = TABLE_MAX_HEIGHT;

    column_widths = vector_i64(table_width);

    // Calculate each column maximum width
    for (u64_t i = 0; i < table_width; i++)
    {
        // First check the column name
        width = strlen(symbols_get(header[i]));
        as_vector_i64(&column_widths)[i] = width;

        // Then traverse column until maximum height limit
        for (u64_t j = 0; j < table_height; j++)
        {
            value_t *column = &as_list(columns)[i];

            switch (column->type)
            {
            case TYPE_I64:
                s = str_fmt(limit, "%lld", as_vector_i64(column)[j]);
                break;
            case TYPE_F64:
                s = str_fmt(limit, "%.*f", F64_PRECISION, as_vector_f64(column)[j]);
                break;
            case TYPE_SYMBOL:
                s = str_fmt(limit, "%s", symbols_get(as_vector_symbol(column)[j]));
                break;
            default:
                s = value_fmt_ind(indent, limit - indent, &as_list(column)[j]);
                break;
            }

            formatted_columns[i][j] = s;
            width = strlen(s);
            if (width > as_vector_i64(&column_widths)[i])
                as_vector_i64(&column_widths)[i] = width;
        }
    }

    // Print table header
    for (u64_t i = 0; i < table_width; i++)
    {
        width = as_vector_i64(&column_widths)[i];
        s = symbols_get(header[i]);
        width = width - strlen(s) + 2;
        offset += str_fmt_into(0, offset, &str, "%s%*.*s|", s, width, width, PADDING);
    }

    // Print table header separator
    offset += str_fmt_into(0, offset, &str, "\n+");

    for (u64_t i = 0; i < table_width; i++)
    {
        width = as_vector_i64(&column_widths)[i] + 2;
        offset += str_fmt_into(0, offset, &str, "%*.*s+", width, width, TABLE_HEADER_SEPARATOR);
    }

    // Print table content
    for (u64_t j = 0; j < table_height; j++)
    {
        offset += str_fmt_into(0, offset, &str, "\n|");

        for (u64_t i = 0; i < table_width; i++)
        {
            width = as_vector_i64(&column_widths)[i] + 1;
            s = formatted_columns[i][j];
            offset += str_fmt_into(0, offset, &str, " %s%*.*s|", s, width - strlen(s), width - strlen(s), PADDING);
            // Free formatted column
            rayforce_free(s);
        }
    }

    return str;
}

str_t error_fmt(u32_t indent, u32_t limit, value_t *value)
{
    value_t code = dict_get(value, symbol("code"));
    value_t message = dict_get(value, symbol("message"));

    return str_fmt(0, "** [E%.3d] error: %s", code.i64, as_string(&message));
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
    case TYPE_TABLE:
        return table_fmt(indent, limit, value);
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
