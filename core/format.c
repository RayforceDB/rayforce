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
#include "lambda.h"
#include "timestamp.h"

#define MAX_ROW_WIDTH 80
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
        s = rf_realloc(*dst, *len);

        if (s == NULL)
        {
            rf_free(*dst);
            panic("str_fmt_into: OOM");
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
                rf_free(*dst);

            panic("str_fmt_into: OOM");
        }

        if (n < size)
            break;

        if (limit > 0)
        {
            *offset += size - 1;
            return n;
        }

        size = n + 1;
        s = rf_realloc(*dst, size);

        if (s == NULL)
        {
            rf_free(*dst);
            panic("str_fmt_into: OOM");
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
    str_t p = rf_malloc(size), s;

    while (1)
    {
        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
        {
            rf_free(p);
            panic("str_fmt_into: OOM");
        }

        if (n < size)
            break;

        if (limit > 0)
            return p;

        size = n + 1;
        s = rf_realloc(p, size);

        if (s == NULL)
        {
            rf_free(p);
            panic("str_fmt_into: OOM");
        }

        p = s;
    }

    return p;
}

i32_t bool_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == 0)
        return str_fmt_into(dst, len, offset, limit, "false");
    else
        return str_fmt_into(dst, len, offset, limit, "true");
}

i32_t i64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "%s", "0i");
    else
        return str_fmt_into(dst, len, offset, limit, "%lld", val);
}

i32_t f64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, f64_t val)
{
    if (rfi_is_nan(val))
        return str_fmt_into(dst, len, offset, limit, "0f");
    else
        return str_fmt_into(dst, len, offset, limit, "%.*f", F64_PRECISION, val);
}

i32_t symbol_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "0s");

    i32_t n = str_fmt_into(dst, len, offset, limit, "%s", symbols_get(val));

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t ts_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "0t");

    timestamp_t ts = rf_timestamp_from_i64(val);
    i32_t n = str_fmt_into(dst, len, offset, limit, "%.4d.%.2d.%.2dD%.2d:%.2d:%.2d.%.9d",
                           ts.year, ts.month, ts.day, ts.hours, ts.mins, ts.secs, ts.nanos);

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t guid_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, guid_t *guid)
{
    u8_t *uuid_buffer = (u8_t *)guid;

    if (uuid_buffer == NULL)
        return str_fmt_into(dst, len, offset, limit, "0g");

    i32_t n = str_fmt_into(dst, len, offset, limit, "%02hhx%02hhx%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                           uuid_buffer[0], uuid_buffer[1], uuid_buffer[2], uuid_buffer[3],
                           uuid_buffer[4], uuid_buffer[5],
                           uuid_buffer[6], uuid_buffer[7],
                           uuid_buffer[8], uuid_buffer[9],
                           uuid_buffer[10], uuid_buffer[11], uuid_buffer[12], uuid_buffer[13], uuid_buffer[14], uuid_buffer[15]);

    return n;
}

i32_t vector_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, rf_object object)
{
    if (object->len == 0)
        return str_fmt_into(dst, len, offset, limit, "[]");

    i32_t i, n = str_fmt_into(dst, len, offset, limit, "["), indent = 0;
    i64_t l;
    rf_object v = NULL;

    if (n > limit)
        return n;

    l = object->len;

    for (i = 0; i < l; i++)
    {
        v = vector_get(object, i);
        n += rf_object_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, &v);
        drop(&v);

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

i32_t list_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object object)
{
    i32_t i, n, list_height = object->len;

    if (list_height == 0)
        return str_fmt_into(dst, len, offset, limit, "()");

    n = str_fmt_into(dst, len, offset, limit, "(");

    if (list_height > LIST_MAX_HEIGHT)
        list_height = LIST_MAX_HEIGHT;

    indent += 2;

    for (i = 0; i < list_height; i++)
    {
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s", indent, indent, PADDING));
        maxn(n, rf_object_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, &as_list(object)[i]));
    }

    if (list_height < (i32_t)object->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s..", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s)", indent, indent, PADDING));
    return n;
}

i32_t string_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, rf_object object)
{
    if (object->ptr == NULL)
        return str_fmt_into(dst, len, offset, limit, "\"\"");

    i32_t n = str_fmt_into(dst, len, offset, limit, "\"%s\"", as_string(object));

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t dict_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object object)
{
    rf_object keys = as_list(object)[0], vals = as_list(object)[1], v;
    i32_t i, n, dict_height = keys->len;

    if (dict_height == 0)
        return str_fmt_into(dst, len, offset, limit, "{}");

    n = str_fmt_into(dst, len, offset, limit, "{");

    if (dict_height > LIST_MAX_HEIGHT)
        dict_height = LIST_MAX_HEIGHT;

    indent += 2;

    for (i = 0; i < dict_height; i++)
    {
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s", indent, indent, PADDING));
        v = vector_get(keys, i);
        maxn(n, rf_object_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, &v));
        drop(&v);

        n += str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, ": ");

        v = vector_get(vals, i);
        maxn(n, rf_object_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, &v));
        drop(&v);
    }

    if (dict_height < (i32_t)keys->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s..", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, limit, "\n%*.*s}", indent, indent, PADDING));

    return n;
}

i32_t table_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, rf_object object)
{
    i64_t *header = as_Symbol(as_list(object)[0]);
    rf_object columns = as_list(object)[1], column_widths, c;
    i32_t table_width, table_height;
    str_t s, formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL}};
    i32_t i, j, l, o, n = str_fmt_into(dst, len, offset, 0, "|");

    table_width = (as_list(object)[0])->len;
    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    table_height = (as_list(columns)[0])->len;
    if (table_height > TABLE_MAX_HEIGHT)
        table_height = TABLE_MAX_HEIGHT;

    column_widths = I64(table_width);

    // Calculate each column maximum width
    for (i = 0; i < table_width; i++)
    {
        // First check the column name
        n = strlen(symbols_get(header[i]));
        as_I64(column_widths)[i] = n;

        // Then traverse column until maximum height limit
        for (j = 0; j < table_height; j++)
        {
            rf_object column = as_list(columns)[i];
            s = NULL;
            l = 0;
            o = 0;

            c = vector_get(column, j);
            maxn(n, rf_object_fmt_into(&s, &l, &o, 0, 31, &c));
            drop(&c);

            formatted_columns[i][j] = s;
            maxn(as_I64(column_widths)[i], n);
        }
    }

    // Print table header
    for (i = 0; i < table_width; i++)
    {
        n = as_I64(column_widths)[i];
        s = symbols_get(header[i]);
        n = n - strlen(s);
        str_fmt_into(dst, len, offset, 0, " %s%*.*s |", s, n, n, PADDING);
    }

    if ((as_list(object)[0])->len > TABLE_MAX_WIDTH)
        str_fmt_into(dst, len, offset, 0, " ..");

    // Print table header separator
    str_fmt_into(dst, len, offset, 0, "\n%*.*s+", indent, indent, PADDING);
    for (i = 0; i < table_width; i++)
    {
        n = as_I64(column_widths)[i] + 2;
        str_fmt_into(dst, len, offset, 0, "%*.*s+", n, n, TABLE_HEADER_SEPARATOR);
    }

    // Print table content
    for (j = 0; j < table_height; j++)
    {
        str_fmt_into(dst, len, offset, 0, "\n%*.*s|", indent, indent, PADDING);

        for (i = 0; i < table_width; i++)
        {
            n = as_I64(column_widths)[i] + 1;
            s = formatted_columns[i][j];
            n = n - strlen(s);
            str_fmt_into(dst, len, offset, 0, " %s%*.*s|", s, n, n, PADDING);
            // Free formatted column
            rf_free(s);
        }
    }

    drop(&column_widths);

    if (table_height < (i32_t)(as_list(columns)[0])->len)
        str_fmt_into(dst, len, offset, 0, "\n..");

    return n;
}

i32_t error_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, rf_object error)
{
    UNUSED(limit);

    return str_fmt_into(dst, len, offset, limit, "** [E%.3d] error: %s", error->code, as_string(error));
}

i32_t internal_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, rf_object object)
{
    rf_object functions = &runtime_get()->env.functions;
    i64_t id, sym;

    id = vector_find(&as_list(functions)[1], object);
    sym = as_Symbol(as_list(functions)[0])[id];

    return symbol_fmt_into(dst, len, offset, limit, sym);
}

i32_t lambda_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, rf_object object)
{
    UNUSED(limit);
    UNUSED(object);

    // lambda_t *lambda = as_lambda(rf_object);

    return str_fmt_into(dst, len, offset, 0, "<lambda>");
}

i32_t rf_object_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object object)
{
    switch (object->type)
    {
    case -TYPE_BOOL:
        return bool_fmt_into(dst, len, offset, limit, object->bool);
    case -TYPE_I64:
        return i64_fmt_into(dst, len, offset, limit, object->i64);
    case -TYPE_F64:
        return f64_fmt_into(dst, len, offset, limit, object->f64);
    case -TYPE_SYMBOL:
        return symbol_fmt_into(dst, len, offset, limit, object->i64);
    case -TYPE_TIMESTAMP:
        return ts_fmt_into(dst, len, offset, limit, object->i64);
    // case -TYPE_GUID:
    //     return guid_fmt_into(dst, len, offset, limit, object->guid);
    case -TYPE_CHAR:
        return str_fmt_into(dst, len, offset, limit, "'%c'", object->schar ? object->schar : 1);
    case TYPE_BOOL:
        return vector_fmt_into(dst, len, offset, limit, object);
    case TYPE_I64:
        return vector_fmt_into(dst, len, offset, limit, object);
    case TYPE_F64:
        return vector_fmt_into(dst, len, offset, limit, object);
    case TYPE_SYMBOL:
        return vector_fmt_into(dst, len, offset, limit, object);
    case TYPE_TIMESTAMP:
        return vector_fmt_into(dst, len, offset, limit, object);
    case TYPE_GUID:
        return vector_fmt_into(dst, len, offset, limit, object);
    case TYPE_CHAR:
        return string_fmt_into(dst, len, offset, limit, object);
    case TYPE_LIST:
        return list_fmt_into(dst, len, offset, indent, limit, object);
    case TYPE_DICT:
        return dict_fmt_into(dst, len, offset, indent, limit, object);
    case TYPE_TABLE:
        return table_fmt_into(dst, len, offset, indent, object);
    case TYPE_UNARY:
    case TYPE_BINARY:
    case TYPE_VARY:
        return internal_fmt_into(dst, len, offset, limit, object);
    case TYPE_LAMBDA:
        return lambda_fmt_into(dst, len, offset, limit, object);
    case TYPE_ERROR:
        return error_fmt_into(dst, len, offset, limit, object);
    default:
        return str_fmt_into(dst, len, offset, limit, "null");
    }
}

/*
 * Format an object into a string
 */
str_t rf_object_fmt(rf_object object)
{
    i32_t len = 0, offset = 0, limit = MAX_ROW_WIDTH;
    str_t dst = NULL;
    rf_object_fmt_into(&dst, &len, &offset, 0, limit, object);
    if (dst == NULL)
        panic("format: returns null");

    return dst;
}

/*
 * Format a list of objects into a string
 * using format string as a template with
 * '%' placeholders.
 */
str_t rf_object_fmt_n(rf_object x, u32_t n)
{
    u32_t i;
    i32_t l = 0, o = 0, sz = 0;
    str_t s = NULL, p, start = NULL, end = NULL;
    rf_object b = x;

    if (n == 0)
        return NULL;

    if (n == 1)
        return rf_object_fmt(b);

    if (b->type != TYPE_CHAR)
        return NULL;

    p = as_string(b);
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
                rf_free(s);

            return NULL;
        }

        if (end > start)
            str_fmt_into(&s, &l, &o, (end - start) + 1, "%s", start);

        sz -= (end + 1 - start);
        start = end + 1;

        rf_object_fmt_into(&s, &l, &o, 0, 0, b);
    }

    if (sz > 0 && memchr(start, '%', sz))
    {
        if (s)
            rf_free(s);

        return NULL;
    }

    str_fmt_into(&s, &l, &o, end - start, "%s", start);

    return s;
}

null_t print_error(rf_object error, str_t filename, str_t source, u32_t len)
{
    const str_t PADDING = "                                                  ";
    u16_t line_number = 0, i, l;
    str_t start = source;
    str_t end = NULL;
    str_t error_desc, lf = "", msg, p;
    // TODO!!!!!
    span_t span = (span_t){0};

    switch (error->code)
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
    case ERR_ALLOC:
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
    {
        printf("%s** [E%.3d] error%s: %s\n %s-->%s %s:%d:%d\n    %s %s %s\n", TOMATO, error->code, RESET,
               error_desc, CYAN, RESET, filename, span.end_line, span.end_column, TOMATO, as_string(error), RESET);
        return;
    }

    printf("%s** [E%.3d] error%s: %s\n %s-->%s %s:%d:%d\n    %s|%s\n", TOMATO, error->code, RESET,
           error_desc, CYAN, RESET, filename, span.end_line, span.end_column, CYAN, RESET);

    while (1)
    {
        end = strchr(start, '\n');
        if (end == NULL)
        {
            end = source + len;
            lf = "\n";
        }

        u32_t line_len = end - start + 1;

        if (line_number >= span.start_line && line_number <= span.end_line)
        {
            printf("%.3d %s|%s %.*s", line_number + 1, CYAN, RESET, line_len, start);

            // Print the arrow or span for the error
            if (span.start_line == span.end_line)
            {
                printf("%s    %s|%s ", lf, CYAN, RESET);
                for (i = 0; i < span.start_column; i++)
                    printf(" ");

                for (i = span.start_column; i <= span.end_column; i++)
                    printf("%s^%s", TOMATO, RESET);

                l = 0;
                msg = as_string(error);
                p = strtok(msg, "\n");
                while (p != NULL)
                {
                    if (!l)
                        printf("%*.*s %s%s%s\n", l, l, PADDING, TOMATO, p, RESET);
                    else
                        printf("%*.*s %s%s%s\n", l, l, PADDING, YELLOW, p, RESET);
                    p = strtok(NULL, "\n");
                    l = span.end_column - span.start_column + 8;
                }
            }
            else
            {
                if (line_number == span.start_line)
                {
                    printf("    %s|%s ", CYAN, RESET);
                    for (i = 0; i < span.start_column; i++)
                        printf(" ");

                    printf("%s^%s\n", TOMATO, RESET);
                }
                else if (line_number == span.end_line)
                {
                    for (i = 0; i < span.end_column + 6; i++)
                        printf(" ");

                    printf("%s^ %s%s\n", TOMATO, as_string(error), RESET);
                }
            }
        }

        if (line_number > span.end_line)
            break;

        line_number++;
        start = end + 1;
    }
}
