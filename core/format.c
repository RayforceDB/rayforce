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
#include "function.h"

#define MAX_ROW_WIDTH 120
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 2
#define TABLE_MAX_WIDTH 10  // Maximum number of columns
#define TABLE_MAX_HEIGHT 10 // Maximum number of rows
#define LIST_MAX_HEIGHT 5   // Maximum number of list/dict rows
#define maxn(n, e)         \
    {                      \
        i32_t k = e;       \
        n = n > k ? n : k; \
    }

const str_t PADDING = "                                                                                                   ";
const str_t TABLE_SEPARATOR = " | ";
const str_t TABLE_HEADER_SEPARATOR = "------------------------------------------------------------------------------------";

extern i32_t rf_object_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object);

/*
 * return n:
 * n > limit - truncated
 * n < limit - fits into buffer
 * n < 0 - error
 */
i32_t str_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p;

    // If ptr is NULL, realloc behaves like malloc.
    if (*len <= (size + *offset))
    {
        *len += size + 1;
        *dst = rf_realloc(*dst, *len);

        if (*dst == NULL)
            panic("str_fmt_into: OOM");
    }

    // debug("LEN: %d, OFFSET: %d, SIZE: %d FREE: %d", *len, *offset, size, *len - *offset);

    while (1)
    {
        p = *dst + *offset;

        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
            panic("str_fmt_into: OOM");

        if (n < size)
            break;

        if (limit > 0)
        {
            *offset += size - 1;
            return n;
        }

        size = n + 1;
        *dst = rf_realloc(*dst, size);

        if (*dst == NULL)
            panic("str_fmt_into: OOM");

        *len += size;
    }

    *offset += n;
    return n;
}

str_t str_fmt(i32_t limit, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p = rf_malloc(size);

    while (1)
    {
        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
            panic("str_fmt_into: OOM");

        if (n < size)
            break;

        if (limit > 0)
            return p;

        size = n + 1;
        p = rf_realloc(p, size);

        if (p == NULL)
            panic("str_fmt_into: OOM");
    }

    return p;
}

i32_t bool_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, i64_t val)
{
    if (val == 0)
        return str_fmt_into(dst, len, offset, limit, "%*.*s%s", indent, indent, PADDING, "false");
    else
        return str_fmt_into(dst, len, offset, limit, "%*.*s%s", indent, indent, PADDING, "true");
}

i32_t i64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "%*.*s%s", indent, indent, PADDING, "0i");
    else
        return str_fmt_into(dst, len, offset, limit, "%*.*s%lld", indent, indent, PADDING, val);
}

i32_t f64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, f64_t val)
{
    if (rf_is_nan(val))
        return str_fmt_into(dst, len, offset, limit, "%*.*s%s", indent, indent, PADDING, "0f");
    else
        return str_fmt_into(dst, len, offset, limit, "%*.*s%.*f", indent, indent, PADDING, F64_PRECISION, val);
}

i32_t symbol_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, i64_t val)
{
    i32_t n = str_fmt_into(dst, len, offset, limit, "%*.*s%s", indent, indent, PADDING, symbols_get(val));

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t vector_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    if (rf_object->adt->len == 0)
        return str_fmt_into(dst, len, offset, limit, "%*.*s[]", indent, indent, PADDING);

    i32_t i, n = str_fmt_into(dst, len, offset, limit, "%*.*s[", indent, indent, PADDING);

    if (n > limit)
        return n;

    for (i = 0; i < rf_object->adt->len; i++)
    {
        if (rf_object->type == TYPE_I64)
            n += i64_fmt_into(dst, len, offset, 0, limit, as_vector_i64(rf_object)[i]);
        else if (rf_object->type == TYPE_F64)
            n += f64_fmt_into(dst, len, offset, 0, limit, as_vector_f64(rf_object)[i]);
        else if (rf_object->type == TYPE_SYMBOL)
            n += symbol_fmt_into(dst, len, offset, 0, limit, as_vector_i64(rf_object)[i]);

        if (n > limit)
            break;

        if (i < rf_object->adt->len - 1)
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

i32_t list_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    if (rf_object->adt == NULL)
        return str_fmt_into(dst, len, offset, limit, "%*.*s()", indent, indent, PADDING);

    i32_t i, n = str_fmt_into(dst, len, offset, limit, "%*.*s(\n", indent, indent, PADDING),
             list_height = rf_object->adt->len;

    if (list_height > LIST_MAX_HEIGHT)
        list_height = LIST_MAX_HEIGHT;

    indent += 2;

    for (i = 0; i < list_height; i++)
    {
        maxn(n, rf_object_fmt_into(dst, len, offset, indent, MAX_ROW_WIDTH, &as_list(rf_object)[i]));
        maxn(n, str_fmt_into(dst, len, offset, 2, "\n"));
    }

    if (list_height < rf_object->adt->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "%*.*s..\n", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, 0, "%*.*s)", indent, indent, PADDING));
    return n;
}

i32_t string_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    if (rf_object->adt == NULL)
        return str_fmt_into(dst, len, offset, limit, "\"\"");

    i32_t n = str_fmt_into(dst, len, offset, limit, "%*.*s\"%s\"", indent, indent, PADDING, as_string(rf_object));

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    return n;
}

i32_t dict_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    rf_object_t *keys = &as_list(rf_object)[0], *vals = &as_list(rf_object)[1];
    i32_t i, m, n = str_fmt_into(dst, len, offset, limit, "%*.*s{\n", indent, indent, PADDING),
                dict_height = keys->adt->len;

    if (dict_height > LIST_MAX_HEIGHT)
        dict_height = LIST_MAX_HEIGHT;

    indent += 2;

    for (i = 0; i < dict_height; i++)
    {
        m = 0;

        // Dispatch keys type
        switch (keys->type)
        {
        case TYPE_I64:
            m += i64_fmt_into(dst, len, offset, indent, limit, as_vector_i64(keys)[i]);
            break;
        case TYPE_F64:
            m += f64_fmt_into(dst, len, offset, indent, limit, as_vector_f64(keys)[i]);
            break;
        case TYPE_SYMBOL:
            m += symbol_fmt_into(dst, len, offset, indent, limit, as_vector_symbol(keys)[i]);
            break;
        default:
            m += rf_object_fmt_into(dst, len, offset, indent, limit, &as_list(keys)[i]);
            break;
        }

        m += str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, ": ");

        // Dispatch rf_objects type
        switch (vals->type)
        {
        case TYPE_I64:
            m += i64_fmt_into(dst, len, offset, 0, limit, as_vector_i64(vals)[i]);
            break;
        case TYPE_F64:
            m += f64_fmt_into(dst, len, offset, 0, limit, as_vector_f64(vals)[i]);
            break;
        case TYPE_SYMBOL:
            m += symbol_fmt_into(dst, len, offset, 0, limit, as_vector_symbol(vals)[i]);
            break;
        default:
            m += rf_object_fmt_into(dst, len, offset, 0, limit, &as_list(vals)[i]);
            break;
        }

        m += str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, "\n");
        maxn(n, m);
    }

    if (dict_height < keys->adt->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "%*.*s..\n", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, limit, "%*.*s}", indent, indent, PADDING));

    return n;
}

i32_t table_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    UNUSED(indent);
    UNUSED(limit);

    i64_t *header = as_vector_symbol(&as_list(rf_object)[0]);
    rf_object_t *columns = &as_list(rf_object)[1], column_widths;
    i32_t table_width, table_height;
    str_t s, formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL}};
    i32_t i, j, l, o, n = str_fmt_into(dst, len, offset, 0, "|");

    table_width = (&as_list(rf_object)[0])->adt->len;
    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    table_height = (&as_list(columns)[0])->adt->len;
    if (table_height > TABLE_MAX_HEIGHT)
        table_height = TABLE_MAX_HEIGHT;

    column_widths = vector_i64(table_width);

    // Calculate each column maximum width
    for (i = 0; i < table_width; i++)
    {
        // First check the column name
        n = strlen(symbols_get(header[i]));
        as_vector_i64(&column_widths)[i] = n;

        // Then traverse column until maximum height limit
        for (j = 0; j < table_height; j++)
        {
            rf_object_t *column = &as_list(columns)[i];
            s = NULL;
            l = 0;
            o = 0;

            switch (column->type)
            {
            case TYPE_I64:
                maxn(n, i64_fmt_into(&s, &l, &o, 0, MAX_ROW_WIDTH, as_vector_i64(column)[j]));
                break;
            case TYPE_F64:
                maxn(n, f64_fmt_into(&s, &l, &o, 0, MAX_ROW_WIDTH, as_vector_f64(column)[j]));
                break;
            case TYPE_SYMBOL:
                maxn(n, symbol_fmt_into(&s, &l, &o, 0, MAX_ROW_WIDTH, as_vector_symbol(column)[j]));
                break;
            default:
                maxn(n, rf_object_fmt_into(&s, &l, &o, 0, MAX_ROW_WIDTH, &as_list(column)[j]));
                break;
            }

            formatted_columns[i][j] = s;
            maxn(as_vector_i64(&column_widths)[i], n);
        }
    }

    // Print table header
    for (i = 0; i < table_width; i++)
    {
        n = as_vector_i64(&column_widths)[i];
        s = symbols_get(header[i]);
        n = n - strlen(s);
        str_fmt_into(dst, len, offset, 0, " %s%*.*s |", s, n, n, PADDING);
    }

    // Print table header separator
    str_fmt_into(dst, len, offset, 0, "\n+");
    for (i = 0; i < table_width; i++)
    {
        n = as_vector_i64(&column_widths)[i] + 2;
        str_fmt_into(dst, len, offset, 0, "%*.*s+", n, n, TABLE_HEADER_SEPARATOR);
    }

    // Print table content
    for (j = 0; j < table_height; j++)
    {
        str_fmt_into(dst, len, offset, 0, "\n|");

        for (i = 0; i < table_width; i++)
        {
            n = as_vector_i64(&column_widths)[i] + 1;
            s = formatted_columns[i][j];
            n = n - strlen(s);
            str_fmt_into(dst, len, offset, 0, " %s%*.*s|", s, n, n, PADDING);
            // Free formatted column
            rf_free(s);
        }
    }

    return n;
}

i32_t error_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *error)
{
    UNUSED(limit);
    UNUSED(indent);

    return str_fmt_into(dst, len, offset, 0, "** [E%.3d] error: %s", error->adt->code, as_string(error));
}

i32_t function_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    UNUSED(limit);
    UNUSED(indent);
    UNUSED(rf_object);

    // function_t *function = as_function(rf_object);

    return str_fmt_into(dst, len, offset, 0, "<function>");
}

i32_t rf_object_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    switch (rf_object->type)
    {
    case -TYPE_BOOL:
        return bool_fmt_into(dst, len, offset, indent, limit, rf_object->bool);
    case -TYPE_I64:
        return i64_fmt_into(dst, len, offset, indent, limit, rf_object->i64);
    case -TYPE_F64:
        return f64_fmt_into(dst, len, offset, indent, limit, rf_object->f64);
    case -TYPE_SYMBOL:
        return symbol_fmt_into(dst, len, offset, indent, limit, rf_object->i64);
    case TYPE_I64:
        return vector_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_F64:
        return vector_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_SYMBOL:
        return vector_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_STRING:
        return string_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_LIST:
        return list_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_DICT:
        return dict_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_TABLE:
        return table_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_FUNCTION:
        return function_fmt_into(dst, len, offset, indent, limit, rf_object);
    case TYPE_ERROR:
        return error_fmt_into(dst, len, offset, indent, limit, rf_object);
    default:
        return str_fmt_into(dst, len, offset, limit, "null");
    }
}

/*
 * Format an object into a string
 */
str_t rf_object_fmt(rf_object_t *rf_object)
{
    i32_t len = 0, offset = 0, limit = MAX_ROW_WIDTH;
    str_t dst = NULL;
    rf_object_fmt_into(&dst, &len, &offset, 0, limit, rf_object);
    if (dst == NULL)
        panic("format: returns null");

    return dst;
}

/*
 * Format a list of objects into a string
 * using format string as a template with
 * '%' placeholders.
 */
str_t rf_object_fmt_n(rf_object_t *x, u32_t n)
{
    u32_t i;
    i32_t l = 0, o = 0;
    str_t s = NULL, p, tok = NULL;

    if (n == 0)
        return NULL;

    if (n == 1)
        return rf_object_fmt(x);

    if (x->type != TYPE_STRING)
        return NULL;

    p = as_string(x);
    tok = strtok(p, "%");

    for (i = 1; i < n; i++)
    {
        if (tok)
            str_fmt_into(&s, &l, &o, 0, tok);

        tok = strtok(NULL, "%");

        if (!tok && i < n - 1)
        {
            if (s)
                rf_free(s);

            return NULL;
        }

        rf_object_fmt_into(&s, &l, &o, 0, 0, x + i);
    }

    return s;
}
