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
#include "debuginfo.h"
#include "runtime.h"
#include "ops.h"

#define MAX_I64_WIDTH 20
#define MAX_ROW_WIDTH MAX_I64_WIDTH * 2
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 4
#define TABLE_MAX_WIDTH 10  // Maximum number of columns
#define TABLE_MAX_HEIGHT 10 // Maximum number of rows

const str_t PADDING = "                                                                                                   ";
const str_t TABLE_SEPARATOR = " | ";
const str_t TABLE_HEADER_SEPARATOR = "------------------------------------------------------------------------------------";

extern str_t object_fmt_ind(i32_t indent, i32_t limit, rf_object_t *object);

extern i32_t str_fmt_into(i32_t limit, i32_t offset, str_t *dst, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p;

    n = strlen(*dst);

    if (n < (size + offset))
    {
        size = size + offset + n;
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

extern str_t str_fmt(i32_t limit, str_t fmt, ...)
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

str_t i64_fmt(i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return "0i";
    else
        return str_fmt(limit, "%lld", val);
}

str_t f64_fmt(i32_t limit, f64_t val)
{
    if (rf_is_nan(val))
        return "0f";
    else
        return str_fmt(limit, "%.*f", F64_PRECISION, val);
}

str_t vector_fmt(i32_t limit, rf_object_t *object)
{
    if (!limit)
        return "";

    if (object->adt.len == 0)
        return str_fmt(3, "[]");

    str_t str, buf;
    i64_t len = 0, remains = limit, i;
    i8_t v_type = object->type;

    str = buf = (str_t)rayforce_malloc(limit + FORMAT_TRAILER_SIZE);
    len = snprintf(buf, remains, "[");

    buf += len;
    remains -= len;

    for (i = 0; i < object->adt.len; i++)
    {
        if (v_type == TYPE_I64)
        {
            if (as_vector_i64(object)[i] == NULL_I64)
                len = snprintf(buf, remains, "0i ");
            else
                len = snprintf(buf, remains, "%lld ", as_vector_i64(object)[i]);
        }
        else if (v_type == TYPE_F64)
        {
            if (as_vector_f64(object)[i] != as_vector_f64(object)[i])
                len = snprintf(buf, remains, "0f ");
            else
                len = snprintf(buf, remains, "%.*f ", F64_PRECISION, as_vector_f64(object)[i]);
        }
        else if (v_type == TYPE_SYMBOL)
            len = snprintf(buf, remains, "%s ", symbols_get(as_vector_symbol(object)[i]));

        if (len < 0)
        {
            rayforce_free(str);
            return "";
        }

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

str_t list_fmt(i32_t indent, i32_t limit, rf_object_t *object)
{
    if (!limit)
        return "";

    if (object->adt.ptr == NULL)
        return str_fmt(limit, "null");

    str_t s, str = str_fmt(limit, "(");
    i32_t offset = 1, i;

    indent += 2;

    for (i = 0; i < object->adt.len; i++)
    {
        s = object_fmt_ind(indent, limit - indent, ((rf_object_t *)object->adt.ptr) + i);
        offset += str_fmt_into(0, offset, &str, "\n%*.*s%s", indent, indent, PADDING, s);
        rayforce_free(s);
    }

    str_fmt_into(0, offset, &str, "\n%*.*s)", indent - 2, indent - 2, PADDING);
    return str;
}

str_t string_fmt(i32_t indent, i32_t limit, rf_object_t *object)
{
    UNUSED(indent);

    if (!limit)
        return "";

    if (object->adt.ptr == NULL)
        return str_fmt(0, "\"\"");

    return str_fmt(object->adt.len + 3, "\"%s\"", object->adt.ptr);
}

str_t dict_fmt(i32_t indent, i32_t limit, rf_object_t *object)
{
    if (!limit)
        return "";

    rf_object_t *keys = &as_list(object)[0], *vals = &as_list(object)[1];
    str_t k, v, str = str_fmt(limit, "{");
    i32_t offset = 1, i;

    indent += 2;

    for (i = 0; i < keys->adt.len; i++)
    {
        // Dispatch keys type
        switch (keys->type)
        {
        case TYPE_I64:
            k = i64_fmt(limit, as_vector_i64(keys)[i]);
            break;
        case TYPE_F64:
            k = str_fmt(limit, "%.*f", F64_PRECISION, as_vector_f64(keys)[i]);
            break;
        case TYPE_SYMBOL:
            k = str_fmt(limit, "%s", symbols_get(as_vector_symbol(keys)[i]));
            break;
        default:
            k = object_fmt_ind(indent, limit - indent, &as_list(keys)[i]);
            break;
        }
        // Dispatch objects type
        switch (vals->type)
        {
        case TYPE_I64:
            v = i64_fmt(limit, as_vector_i64(vals)[i]);
            break;
        case TYPE_F64:
            v = str_fmt(limit, "%.*f", F64_PRECISION, as_vector_f64(vals)[i]);
            break;
        case TYPE_SYMBOL:
            v = str_fmt(limit, "%s", symbols_get(as_vector_symbol(vals)[i]));
            break;
        default:
            v = object_fmt_ind(indent, limit - indent, &as_list(vals)[i]);
            break;
        }

        offset += str_fmt_into(0, offset, &str, "\n%*.*s%s: %s", indent, indent, PADDING, k, v);
        rayforce_free(k);
        rayforce_free(v);
    }

    str_fmt_into(0, offset, &str, "\n%*.*s}", indent - 2, indent - 2, PADDING);
    return str;
}

str_t table_fmt(i32_t indent, i32_t limit, rf_object_t *object)
{
    if (!limit)
        return "";

    i64_t *header = as_vector_symbol(&as_list(object)[0]);
    rf_object_t *columns = &as_list(object)[1], column_widths;
    i32_t table_width, width, table_height;
    str_t str = str_fmt(0, "|"), s;
    str_t formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL}};
    i32_t offset = 1, i, j;

    table_width = (&as_list(object)[0])->adt.len;
    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    table_height = (&as_list(columns)[0])->adt.len;
    if (table_height > TABLE_MAX_HEIGHT)
        table_height = TABLE_MAX_HEIGHT;

    column_widths = vector_i64(table_width);

    // Calculate each column maximum width
    for (i = 0; i < table_width; i++)
    {
        // First check the column name
        width = strlen(symbols_get(header[i]));
        as_vector_i64(&column_widths)[i] = width;

        // Then traverse column until maximum height limit
        for (j = 0; j < table_height; j++)
        {
            rf_object_t *column = &as_list(columns)[i];

            switch (column->type)
            {
            case TYPE_I64:
                s = i64_fmt(limit, as_vector_i64(column)[j]);
                break;
            case TYPE_F64:
                s = f64_fmt(limit, as_vector_f64(column)[j]);
                break;
            case TYPE_SYMBOL:
                s = str_fmt(limit, "%s", symbols_get(as_vector_symbol(column)[j]));
                break;
            default:
                s = object_fmt_ind(indent, limit - indent, &as_list(column)[j]);
                break;
            }

            formatted_columns[i][j] = s;
            width = strlen(s);
            if (width > as_vector_i64(&column_widths)[i])
                as_vector_i64(&column_widths)[i] = width;
        }
    }

    // Print table header
    for (i = 0; i < table_width; i++)
    {
        width = as_vector_i64(&column_widths)[i];
        s = symbols_get(header[i]);
        width = width - strlen(s) + 2;
        offset += str_fmt_into(0, offset, &str, "%s%*.*s|", s, width, width, PADDING);
    }

    // Print table header separator
    offset += str_fmt_into(0, offset, &str, "\n+");

    for (i = 0; i < table_width; i++)
    {
        width = as_vector_i64(&column_widths)[i] + 2;
        offset += str_fmt_into(0, offset, &str, "%*.*s+", width, width, TABLE_HEADER_SEPARATOR);
    }

    // Print table content
    for (j = 0; j < table_height; j++)
    {
        offset += str_fmt_into(0, offset, &str, "\n|");

        for (i = 0; i < table_width; i++)
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

str_t error_fmt(i32_t indent, i32_t limit, rf_object_t *error)
{
    UNUSED(indent);
    UNUSED(limit);
    return str_fmt(0, "** [E%.3d] error: %s", error->adt.code, as_string(error));
}

extern str_t object_fmt_ind(i32_t indent, i32_t limit, rf_object_t *object)
{
    switch (object->type)
    {
    case TYPE_LIST:
        return list_fmt(indent, limit, object);
    case -TYPE_I64:
        return i64_fmt(limit, object->i64);
    case -TYPE_F64:
        return f64_fmt(limit, object->f64);
    case -TYPE_SYMBOL:
        return str_fmt(limit, "%s", symbols_get(object->i64));
    case TYPE_I64:
        return vector_fmt(limit, object);
    case TYPE_F64:
        return vector_fmt(limit, object);
    case TYPE_SYMBOL:
        return vector_fmt(limit, object);
    case TYPE_STRING:
        return string_fmt(indent, limit, object);
    case TYPE_DICT:
        return dict_fmt(indent, limit, object);
    case TYPE_TABLE:
        return table_fmt(indent, limit, object);
    case TYPE_ERROR:
        return error_fmt(indent, limit, object);
    default:
        return str_fmt(limit, "null");
    }
}

extern str_t object_fmt(rf_object_t *object)
{
    i32_t indent = 0, limit = MAX_ROW_WIDTH - FORMAT_TRAILER_SIZE;
    return object_fmt_ind(indent, limit, object);
}

extern str_t type_fmt(i8_t type)
{
    switch (type)
    {
    case -TYPE_I64:
        return str_fmt(0, "i64");
    case -TYPE_F64:
        return str_fmt(0, "f64");
    case -TYPE_SYMBOL:
        return str_fmt(0, "s64");
    case TYPE_I64:
        return str_fmt(0, "i64[]");
    case TYPE_F64:
        return str_fmt(0, "f64[]");
    case TYPE_SYMBOL:
        return str_fmt(0, "s64[]");
    case TYPE_STRING:
        return str_fmt(0, "str");
    case TYPE_LIST:
        return str_fmt(0, "list");
    case TYPE_DICT:
        return str_fmt(0, "dict");
    case TYPE_TABLE:
        return str_fmt(0, "table");
    default:
        return str_fmt(0, "unknown");
    }
}