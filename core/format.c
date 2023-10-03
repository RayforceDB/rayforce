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
#include <stdarg.h>
#include <math.h>
#include "string.h"
#include "format.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"
#include "nfo.h"
#include "runtime.h"
#include "ops.h"
#include "lambda.h"
#include "timestamp.h"
#include "unary.h"
#include "binary.h"
#include "items.h"
#include "io.h"

#define MAX_ROW_WIDTH 800
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 2
#define TABLE_MAX_WIDTH 10  // Maximum number of columns
#define TABLE_MAX_HEIGHT 20 // Maximum number of rows
#define LIST_MAX_HEIGHT 5   // Maximum number of list/dict rows
#define maxn(n, e)         \
    {                      \
        i32_t k = e;       \
        n = n > k ? n : k; \
    }

const str_t PADDING = "                                                                                                   ";
const str_t TABLE_SEPARATOR = " | ";
const str_t TABLE_HEADER_SEPARATOR = "------------------------------------------------------------------------------------";

/*
 * return n:
 * n > limit - truncated
 * n < limit - fits into buffer
 * n < 0 - error
 */
i32_t str_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p, s;

    // If ptr is NULL, realloc behaves like malloc.
    if (*len <= (size + *offset))
    {
        *len += size + 1;
        s = heap_realloc(*dst, *len);

        if (s == NULL)
        {
            heap_free(*dst);
            throw("str_fmt_into: OOM");
        }

        *dst = s;
    }

    while (1)
    {
        p = *dst + *offset;

        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
        {
            if (*dst != NULL)
                heap_free(*dst);

            throw("str_fmt_into: OOM");
        }

        if (n < size)
            break;

        if (limit > 0)
        {
            *offset += size - 1;
            return n;
        }

        size = n + 1;
        s = heap_realloc(*dst, size);

        if (s == NULL)
        {
            heap_free(*dst);
            throw("str_fmt_into: OOM");
        }

        *dst = s;
        *len += size;
    }

    *offset += n;
    return n;
}

str_t str_fmt(i32_t limit, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p = heap_alloc(size), s;

    while (1)
    {
        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
        {
            heap_free(p);
            throw("str_fmt_into: OOM");
        }

        if (n < size)
            break;

        if (limit > 0)
            return p;

        size = n + 1;
        s = heap_realloc(p, size);

        if (s == NULL)
        {
            heap_free(p);
            throw("str_fmt_into: OOM");
        }

        p = s;
    }

    return p;
}

i32_t bool_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, bool_t val)
{
    if (val)
        return str_fmt_into(dst, len, offset, limit, "true");

    return str_fmt_into(dst, len, offset, limit, "false");
}

i32_t byte_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, u8_t val)
{
    return str_fmt_into(dst, len, offset, limit, "0x%02x", val);
}

i32_t i64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "%s", "0i");

    return str_fmt_into(dst, len, offset, limit, "%lld", val);
}

i32_t f64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, f64_t val)
{
    f64_t order;

    if (rfi_is_nan(val))
        return str_fmt_into(dst, len, offset, limit, "0f");

    // Find the order of magnitude of the number to select the appropriate format
    order = log10(val);
    if (val && (order > 6 || order < -4))
        return str_fmt_into(dst, len, offset, limit, "%.*e", 3 * F64_PRECISION, val);

    return str_fmt_into(dst, len, offset, limit, "%.*f", F64_PRECISION, val);
}

i32_t symbol_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "0s");

    i32_t n = str_fmt_into(dst, len, offset, limit, "%s", symtostr(val));

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t ts_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "0t");

    timestamp_t ts = ray_timestamp_from_i64(val);
    i32_t n = str_fmt_into(dst, len, offset, limit, "%.4d.%.2d.%.2dD%.2d:%.2d:%.2d.%.9d",
                           ts.year, ts.month, ts.day, ts.hours, ts.mins, ts.secs, ts.nanos);

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t guid_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, guid_t *val)
{
    u8_t *guid = val->buf;

    if (memcmp(guid, NULL_GUID, 16) == 0)
        return str_fmt_into(dst, len, offset, limit, "0g");

    i32_t n = str_fmt_into(dst, len, offset, limit, "%02hhx%02hhx%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                           guid[0], guid[1], guid[2], guid[3],
                           guid[4], guid[5], guid[6], guid[7],
                           guid[8], guid[9], guid[10], guid[11],
                           guid[12], guid[13], guid[14], guid[15]);

    return n;
}

i32_t error_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj)
{
    i32_t n = 0;
    u16_t line_number = 0, i, l;
    str_t filename, source = NULL, start, end = NULL, error_desc, lf = "", p, msg;
    span_t span;
    obj_t file = null(0);

    // there is a filename
    if (as_list(obj)[3])
    {
        filename = as_string(as_list(obj)[3]);

        // There is no source, so try to get it by reading the file
        if (!as_list(obj)[4])
        {
            file = ray_read(as_list(obj)[3]);

            if (is_error(file))
            {
                drop(file);
                goto nosrc;
            }

            source = as_string(file);
        }
        else
            source = as_string(as_list(obj)[4]);

        start = source;
        span = *(span_t *)&as_list(obj)[2];

        switch (as_list(obj)[0]->i64)
        {
        case ERR_INIT:
            error_desc = "init";
            break;
        case ERR_PARSE:
            error_desc = "parse";
            break;
        case ERR_FORMAT:
            error_desc = "format";
            break;
        case ERR_TYPE:
            error_desc = "type";
            break;
        case ERR_LENGTH:
            error_desc = "length";
            break;
        case ERR_INDEX:
            error_desc = "index";
            break;
        case ERR_HEAP:
            error_desc = "alloc";
            break;
        case ERR_IO:
            error_desc = "io";
            break;
        case ERR_NOT_FOUND:
            error_desc = "not found";
            break;
        case ERR_NOT_EXIST:
            error_desc = "not exist";
            break;
        case ERR_NOT_IMPLEMENTED:
            error_desc = "not implemented";
            break;
        case ERR_STACK_OVERFLOW:
            error_desc = "stack overflow";
            break;
        case ERR_THROW:
            error_desc = "throw";
            break;
        default:
            error_desc = "unknown";
        }

        if (!source)
            return str_fmt_into(dst, len, offset, limit, "%s** [E%.3d] error%s: %s\n %s-->%s %s:%d:%d\n    %s %s %s\n", TOMATO, (i32_t)as_list(obj)[0]->i64, RESET,
                                error_desc, CYAN, RESET, filename, span.end_line + 1, span.end_column + 1, TOMATO, as_string(as_list(obj)[1]), RESET);

        n += str_fmt_into(dst, len, offset, limit, "%s** [E%.3d] error%s: %s\n %s-->%s %s:%d:%d\n    %s|%s\n", TOMATO, (i32_t)as_list(obj)[0]->i64, RESET,
                          error_desc, CYAN, RESET, filename, span.end_line + 1, span.end_column + 1, CYAN, RESET);

        while (1)
        {
            end = strchr(start, '\n');
            if (end == NULL)
            {
                end = source + as_list(obj)[4]->len;
                lf = "\n";
            }

            u32_t line_len = end - start + 1;

            if (line_number >= span.start_line && line_number <= span.end_line)
            {
                n += str_fmt_into(dst, len, offset, limit, "%.3d %s|%s %.*s", line_number + 1, CYAN, RESET, line_len, start);

                // Print the arrow or span for the error
                if (span.start_line == span.end_line)
                {
                    n += str_fmt_into(dst, len, offset, limit, "%s    %s|%s ", lf, CYAN, RESET);
                    for (i = 0; i < span.start_column; i++)
                        n += str_fmt_into(dst, len, offset, limit, " ");

                    for (i = span.start_column; i <= span.end_column; i++)
                        n += str_fmt_into(dst, len, offset, limit, "%s^%s", TOMATO, RESET);

                    l = 0;
                    msg = as_string(as_list(obj)[1]);
                    p = strtok(msg, "\n");
                    while (p != NULL)
                    {
                        if (!l)
                            n += str_fmt_into(dst, len, offset, limit, "%*.*s %s%s%s\n", l, l, PADDING, TOMATO, p, RESET);
                        else
                            n += str_fmt_into(dst, len, offset, limit, "%*.*s %s%s%s\n", l, l, PADDING, YELLOW, p, RESET);
                        p = strtok(NULL, "\n");
                        l = span.end_column - span.start_column + 8;
                    }
                }
                else
                {
                    if (line_number == span.start_line)
                    {
                        n += str_fmt_into(dst, len, offset, limit, "    %s|%s ", CYAN, RESET);
                        for (i = 0; i < span.start_column; i++)
                            n += str_fmt_into(dst, len, offset, limit, " ");

                        n += str_fmt_into(dst, len, offset, limit, "%s^%s\n", TOMATO, RESET);
                    }
                    else if (line_number == span.end_line)
                    {
                        for (i = 0; i < span.end_column + 6; i++)
                            n += str_fmt_into(dst, len, offset, limit, " ");

                        n += str_fmt_into(dst, len, offset, limit, "%s^ %s%s\n", TOMATO, as_string(as_list(obj)[1]), RESET);
                    }
                }
            }

            if (line_number > span.end_line)
                break;

            line_number++;
            start = end + 1;
        }

        drop(file);
        return n;
    }

nosrc:
    return str_fmt_into(dst, len, offset, limit, "** [E%.3d] error: %s",
                        (i32_t)as_list(obj)[0]->i64, as_string(as_list(obj)[1]));
}

i32_t raw_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, obj_t obj, i32_t i)
{
    obj_t idx, res;
    i32_t n;

    switch (obj->type)
    {
    case TYPE_BOOL:
        return bool_fmt_into(dst, len, offset, limit, as_bool(obj)[i]);
    case TYPE_BYTE:
        return byte_fmt_into(dst, len, offset, limit, as_u8(obj)[i]);
    case TYPE_I64:
        return i64_fmt_into(dst, len, offset, limit, as_i64(obj)[i]);
    case TYPE_F64:
        return f64_fmt_into(dst, len, offset, limit, as_f64(obj)[i]);
    case TYPE_SYMBOL:
        return symbol_fmt_into(dst, len, offset, limit, as_symbol(obj)[i]);
    case TYPE_TIMESTAMP:
        return ts_fmt_into(dst, len, offset, limit, as_timestamp(obj)[i]);
    case TYPE_GUID:
        return guid_fmt_into(dst, len, offset, limit, &as_guid(obj)[i]);
    case TYPE_CHAR:
        return str_fmt_into(dst, len, offset, limit, "%c", as_string(obj)[i]);
    case TYPE_LIST:
        return obj_fmt_into(dst, len, offset, indent, limit, false, as_list(obj)[i]);
    case TYPE_ENUM:
    case TYPE_ANYMAP:
        idx = i64(i);
        res = ray_at(obj, idx);
        drop(idx);
        if (is_error(res))
        {
            drop(res);
            return error_fmt_into(dst, len, offset, limit, res);
        }
        n = obj_fmt_into(dst, len, offset, indent, limit, false, res);
        drop(res);
        return n;
    default:
        return str_fmt_into(dst, len, offset, limit, "null");
    }
}

i32_t vector_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj)
{
    if (obj->len == 0)
        return str_fmt_into(dst, len, offset, limit, "[]");

    i32_t i, n = str_fmt_into(dst, len, offset, limit, "["), indent = 0;
    i64_t l;

    if (n > limit)
        return n;

    l = obj->len;

    for (i = 0; i < l; i++)
    {
        n += raw_fmt_into(dst, len, offset, indent, limit, obj, i);
        if (n > limit)
            break;

        if (i + 1 < l)
            n += str_fmt_into(dst, len, offset, limit, " ");

        if (n > limit)
            break;
    }

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 4, "..]");
    else
        n += str_fmt_into(dst, len, offset, 2, "]");

    return n;
}

i32_t list_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, bool_t full, obj_t obj)
{
    i32_t i, n, list_height = obj->len;

    if (list_height == 0)
        return str_fmt_into(dst, len, offset, limit, "()");

    n = str_fmt_into(dst, len, offset, limit, "(");

    if (list_height > LIST_MAX_HEIGHT)
        list_height = LIST_MAX_HEIGHT;

    if (!full)
    {
        for (i = 0; i < list_height - 1; i++)
        {
            maxn(n, obj_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, false, as_list(obj)[i]));
            maxn(n, str_fmt_into(dst, len, offset, 0, " "));
        }

        maxn(n, obj_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, false, as_list(obj)[i]));

        if (list_height < (i32_t)obj->len)
            maxn(n, str_fmt_into(dst, len, offset, 0, ".."));

        maxn(n, str_fmt_into(dst, len, offset, 0, ")"));

        return n;
    }

    indent += 2;

    for (i = 0; i < list_height; i++)
    {
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s", indent, indent, PADDING));
        maxn(n, obj_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, false, as_list(obj)[i]));
    }

    if (list_height < (i32_t)obj->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s..", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s)", indent, indent, PADDING));

    return n;
}

i32_t string_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj)
{
    i32_t n = str_fmt_into(dst, len, offset, limit, "\"%s\"", as_string(obj));

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t enum_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, obj_t obj)
{
    i32_t n;
    obj_t s, e, idx;

    s = ray_key(obj);
    if (enum_val(obj)->len >= TABLE_MAX_HEIGHT)
    {
        limit = TABLE_MAX_HEIGHT;
        idx = i64(TABLE_MAX_HEIGHT);
        e = ray_take(idx, obj);
        drop(idx);
    }
    else
        e = ray_value(obj);

    n = str_fmt_into(dst, len, offset, limit, "'");
    n += obj_fmt_into(dst, len, offset, indent, limit, false, s);
    n += str_fmt_into(dst, len, offset, limit, "#");
    n += obj_fmt_into(dst, len, offset, indent, limit, false, e);

    drop(s);
    drop(e);

    return n;
}

i32_t anymap_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, bool_t full, obj_t obj)
{
    i32_t n;
    obj_t a, idx;

    if (anymap_val(obj)->len >= TABLE_MAX_HEIGHT)
    {
        limit = TABLE_MAX_HEIGHT;
        idx = i64(TABLE_MAX_HEIGHT);
        a = ray_take(idx, obj);
        drop(idx);
    }
    else
        a = ray_value(obj);

    n = obj_fmt_into(dst, len, offset, indent, limit, full, a);

    drop(a);

    return n;
}

i32_t dict_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, bool_t full, obj_t obj)
{
    obj_t keys = as_list(obj)[0], vals = as_list(obj)[1];
    i32_t i, n, dict_height = keys->len;

    if (dict_height == 0)
        return str_fmt_into(dst, len, offset, limit, "{}");

    n = str_fmt_into(dst, len, offset, limit, "{");

    if (dict_height > LIST_MAX_HEIGHT)
        dict_height = LIST_MAX_HEIGHT;

    if (!full)
    {
        for (i = 0; i < dict_height - 1; i++)
        {
            maxn(n, raw_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, keys, i));
            maxn(n, str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, ": "));
            maxn(n, raw_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, vals, i));
            maxn(n, str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, " "));
        }

        maxn(n, raw_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, keys, i));
        maxn(n, str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, ": "));
        maxn(n, raw_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, vals, i));

        if (dict_height < (i32_t)keys->len)
            maxn(n, str_fmt_into(dst, len, offset, 0, "..", indent, indent, PADDING));

        maxn(n, str_fmt_into(dst, len, offset, limit, "}"));

        return n;
    }

    indent += 2;

    for (i = 0; i < dict_height; i++)
    {
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s", indent, indent, PADDING));
        maxn(n, raw_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, keys, i));
        n += str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, ": ");
        maxn(n, raw_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, vals, i));
    }

    if (dict_height < (i32_t)keys->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s..", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, limit, "\n%*.*s}", indent, indent, PADDING));

    return n;
}

i32_t table_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, bool_t full, obj_t obj)
{
    i64_t *header = as_symbol(as_list(obj)[0]);
    obj_t columns = as_list(obj)[1], column_widths;
    str_t s, formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL}};
    i32_t i, j, l, o, n, table_width, table_height;

    if (!full)
    {
        n = str_fmt_into(dst, len, offset, 0, "(table ");
        n += obj_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, false, as_list(obj)[0]);
        n += str_fmt_into(dst, len, offset, indent, " ..)");

        return n;
    }

    n = str_fmt_into(dst, len, offset, 0, "|");

    table_width = (as_list(obj)[0])->len;
    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    table_height = columns->len > 0 ? as_list(columns)[0]->len : 0;
    if (table_height > TABLE_MAX_HEIGHT)
        table_height = TABLE_MAX_HEIGHT;

    column_widths = vector_i64(table_width);

    // Calculate each column maximum width
    for (i = 0; i < table_width; i++)
    {
        // First check the column name
        n = strlen(symtostr(header[i]));

        // Then traverse column until maximum height limit
        if (table_height > 0)
        {
            for (j = 0; j < table_height; j++)
            {
                obj_t column = as_list(columns)[i];
                s = NULL;
                l = 0;
                o = 0;
                raw_fmt_into(&s, &l, &o, 0, 31, column, j);
                formatted_columns[i][j] = s;
                maxn(n, o);
            }
        }

        as_i64(column_widths)[i] = n;
    }

    // Print table header
    for (i = 0; i < table_width; i++)
    {
        n = as_i64(column_widths)[i];
        s = symtostr(header[i]);
        n = n - strlen(s);
        str_fmt_into(dst, len, offset, 0, " %s%*.*s |", s, n, n, PADDING);
    }

    if (as_list(obj)[0]->len > TABLE_MAX_WIDTH)
        str_fmt_into(dst, len, offset, 0, " ..");

    // Print table header separator
    str_fmt_into(dst, len, offset, 0, "\n%*.*s+", indent, indent, PADDING);
    for (i = 0; i < table_width; i++)
    {
        n = as_i64(column_widths)[i] + 2;
        str_fmt_into(dst, len, offset, 0, "%*.*s+", n, n, TABLE_HEADER_SEPARATOR);
    }

    // Print table content
    for (j = 0; j < table_height; j++)
    {
        str_fmt_into(dst, len, offset, 0, "\n%*.*s|", indent, indent, PADDING);

        for (i = 0; i < table_width; i++)
        {
            n = as_i64(column_widths)[i] + 1;
            s = formatted_columns[i][j];
            n = n - strlen(s);
            str_fmt_into(dst, len, offset, 0, " %s%*.*s|", s, n, n, PADDING);
            // Free formatted column
            heap_free(s);
        }
    }

    drop(column_widths);

    if ((table_height > 0) && ((u64_t)table_height < as_list(columns)[0]->len))
        str_fmt_into(dst, len, offset, 0, "\n..");

    return n;
}

i32_t internal_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj)
{
    return str_fmt_into(dst, len, offset, limit, "%s", env_get_internal_name(obj));
}

i32_t lambda_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, obj_t obj)
{
    i32_t n;

    n = str_fmt_into(dst, len, offset, indent, "(fn ");
    n += obj_fmt_into(dst, len, offset, indent, limit, false, as_lambda(obj)->args);
    n += str_fmt_into(dst, len, offset, indent, " ..)");

    return n;
}

i32_t obj_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, bool_t full, obj_t obj)
{
    if (obj == NULL)
        return str_fmt_into(dst, len, offset, limit, "null");

    switch (obj->type)
    {
    case -TYPE_BOOL:
        return bool_fmt_into(dst, len, offset, limit, obj->bool);
    case -TYPE_BYTE:
        return byte_fmt_into(dst, len, offset, limit, obj->u8);
    case -TYPE_I64:
        return i64_fmt_into(dst, len, offset, limit, obj->i64);
    case -TYPE_F64:
        return f64_fmt_into(dst, len, offset, limit, obj->f64);
    case -TYPE_SYMBOL:
        return symbol_fmt_into(dst, len, offset, limit, obj->i64);
    case -TYPE_TIMESTAMP:
        return ts_fmt_into(dst, len, offset, limit, obj->i64);
    case -TYPE_GUID:
        return guid_fmt_into(dst, len, offset, limit, as_guid(obj));
    case -TYPE_CHAR:
        return str_fmt_into(dst, len, offset, limit, "'%c'", obj->vchar ? obj->vchar : 1);
    case TYPE_BOOL:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_BYTE:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_I64:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_F64:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_SYMBOL:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_TIMESTAMP:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_GUID:
        return vector_fmt_into(dst, len, offset, limit, obj);
    case TYPE_CHAR:
        return string_fmt_into(dst, len, offset, limit, obj);
    case TYPE_LIST:
        return list_fmt_into(dst, len, offset, indent, limit, full, obj);
    case TYPE_ENUM:
        return enum_fmt_into(dst, len, offset, indent, limit, obj);
    case TYPE_ANYMAP:
        return anymap_fmt_into(dst, len, offset, indent, limit, full, obj);
    case TYPE_DICT:
        return dict_fmt_into(dst, len, offset, indent, limit, full, obj);
    case TYPE_TABLE:
        return table_fmt_into(dst, len, offset, indent, full, obj);
    case TYPE_UNARY:
    case TYPE_BINARY:
    case TYPE_VARY:
        return internal_fmt_into(dst, len, offset, limit, obj);
    case TYPE_LAMBDA:
        return lambda_fmt_into(dst, len, offset, indent, limit, obj);
    case TYPE_ERROR:
        return error_fmt_into(dst, len, offset, limit, obj);
    default:
        return str_fmt_into(dst, len, offset, limit, "null");
    }
}

/*
 * Format an obj into a string
 */
str_t obj_fmt(obj_t obj)
{
    str_t dst = NULL;
    i32_t len = 0, offset = 0, limit = MAX_ROW_WIDTH;

    if (obj == NULL)
        str_fmt_into(&dst, &len, &offset, limit, "null");
    else
        obj_fmt_into(&dst, &len, &offset, 0, limit, true, obj);

    if (dst == NULL)
        throw("format: returns null");

    return dst;
}

/*
 * Format a list of objs into a string
 * using format string as a template with
 * '%' placeholders.
 */
str_t obj_fmt_n(obj_t *x, u64_t n)
{
    u64_t i;
    i32_t l = 0, o = 0, sz = 0;
    str_t s = NULL, p, start = NULL, end = NULL;
    obj_t *b = x;

    if (n == 0)
        return NULL;

    if (n == 1)
        return obj_fmt(*b);

    if ((*b)->type != TYPE_CHAR)
        return NULL;

    p = as_string(*b);
    sz = strlen(p);
    start = p;
    n -= 1;

    for (i = 0; i < n; i++)
    {
        b += 1;
        end = memchr(start, '%', sz);

        if (!end)
        {
            if (s)
                heap_free(s);

            return NULL;
        }

        if (end > start)
            str_fmt_into(&s, &l, &o, (end - start) + 1, "%s", start);

        sz -= (end + 1 - start);
        start = end + 1;

        obj_fmt_into(&s, &l, &o, 0, 0, true, *b);
    }

    if (sz > 0 && memchr(start, '%', sz))
    {
        if (s)
            heap_free(s);

        return NULL;
    }

    str_fmt_into(&s, &l, &o, end - start, "%s", start);

    return s;
}
