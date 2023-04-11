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
#define F64_PRECISION 2
#define TABLE_MAX_WIDTH 10  // Maximum number of columns
#define TABLE_MAX_HEIGHT 10 // Maximum number of rows
#define chk(n)  \
    if (n != 0) \
    return n

const str_t PADDING = "                                                                                                   ";
const str_t TABLE_SEPARATOR = " | ";
const str_t TABLE_HEADER_SEPARATOR = "------------------------------------------------------------------------------------";

extern i32_t rf_object_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object);

/*
 * return values:
 * 0 - fits into buffer
 * >0 - truncated
 * <0 - error
 */
extern i32_t str_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, str_t fmt, ...)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p;

    size -= FORMAT_TRAILER_SIZE;

    // first call - we need to allocate new string
    if (*len == 0)
    {
        *dst = rf_malloc(size);
        *len = size;
    }
    else if (*len <= (size + *offset))
    {
        if (limit)
            size = *len - *offset - FORMAT_TRAILER_SIZE;
        else
        {
            *len = size + *offset;
            *dst = rf_realloc(*dst, *len);
        }
    }

    if (size <= FORMAT_TRAILER_SIZE)
    {
        p = *dst + *offset;
        snprintf(p, FORMAT_TRAILER_SIZE, "..");
        *offset += FORMAT_TRAILER_SIZE;
        return 1;
    }

    while (1)
    {
        p = *dst + *offset;

        va_list args;
        va_start(args, fmt);
        n = vsnprintf(p, size, fmt, args);
        va_end(args);

        if (n < 0)
            return n;

        if (n < size)
            break;

        if (limit)
        {
            p += size - 1;
            snprintf(p, FORMAT_TRAILER_SIZE, "..");
            *offset += size + FORMAT_TRAILER_SIZE;
            return 1;
        }

        size = n + 1;
        *len += size;
        *dst = rf_realloc(*dst, *len);
    }

    *offset += n;
    return 0;
}

extern str_t str_fmt(i32_t limit, str_t fmt, ...)
{
    str_t s = NULL;
    i32_t len = 0, offset = 0;
    str_fmt_into(&s, &len, &offset, limit, fmt);
    return s;
}

i32_t i64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, len, offset, limit, "%*.*s%lld", indent, indent, PADDING, "0i");
    else
        return str_fmt_into(dst, len, offset, limit, "%*.*s%lld", indent, indent, PADDING, val);
}

i32_t f64_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, f64_t val)
{
    if (rf_is_nan(val))
        return str_fmt_into(dst, len, offset, limit, "%*.*s%.*f", indent, indent, PADDING, F64_PRECISION, "0f");
    else
        return str_fmt_into(dst, len, offset, limit, "%*.*s%.*f", indent, indent, PADDING, F64_PRECISION, val);
}

i32_t symbol_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, i64_t val)
{
    return str_fmt_into(dst, len, offset, limit, "%*.*s%s", indent, indent, PADDING, symbols_get(val));
}

i32_t vector_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    if (rf_object->adt->len == 0)
        return str_fmt_into(dst, len, offset, limit, "%*.*s[]", indent, indent, PADDING);

    i32_t i, n = str_fmt_into(dst, len, offset, limit, "%*.*s[", indent, indent, PADDING);

    for (i = 0; i < rf_object->adt->len; i++)
    {
        if (rf_object->type == TYPE_I64)
            n = i64_fmt_into(dst, len, offset, 0, limit, as_vector_i64(rf_object)[i]);
        else if (rf_object->type == TYPE_F64)
            n = f64_fmt_into(dst, len, offset, 0, limit, as_vector_f64(rf_object)[i]);
        else if (rf_object->type == TYPE_SYMBOL)
            n = symbol_fmt_into(dst, len, offset, 0, limit, as_vector_i64(rf_object)[i]);

        chk(n);

        if (i < rf_object->adt->len - 1)
            n = str_fmt_into(dst, len, offset, limit, " ");

        chk(n);
    }

    str_fmt_into(dst, len, offset, 0, "]");

    return n;
}

i32_t list_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    if (rf_object->adt == NULL)
        return str_fmt_into(dst, len, offset, limit, "%*.*s()", indent, indent, PADDING);

    i32_t i, n = str_fmt_into(dst, len, offset, limit, "%*.*s(\n", indent, indent, PADDING);

    chk(n);

    indent += 2;

    for (i = 0; i < rf_object->adt->len; i++)
    {
        n = rf_object_fmt_into(dst, len, offset, indent, limit, &as_list(rf_object)[i]);
        chk(n);
        n = str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, "\n");
        chk(n);
    }

    indent -= 2;
    return str_fmt_into(dst, len, offset, limit, "%*.*s)", indent, indent, PADDING);
}

i32_t string_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    if (rf_object->adt == NULL)
        return str_fmt_into(dst, len, offset, limit, "\"\"");

    return str_fmt_into(dst, len, offset, limit, "%*.*s\"%s\"", indent, indent, PADDING, as_string(rf_object));
}

i32_t dict_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    rf_object_t *keys = &as_list(rf_object)[0], *vals = &as_list(rf_object)[1];
    i32_t i, n = str_fmt_into(dst, len, offset, limit, "%*.*s{\n", indent, indent, PADDING);

    chk(n);

    indent += 2;

    for (i = 0; i < keys->adt->len; i++)
    {
        // Dispatch keys type
        switch (keys->type)
        {
        case TYPE_I64:
            n = i64_fmt_into(dst, len, offset, indent, limit, as_vector_i64(keys)[i]);
            break;
        case TYPE_F64:
            n = f64_fmt_into(dst, len, offset, indent, limit, as_vector_f64(keys)[i]);
            break;
        case TYPE_SYMBOL:
            n = symbol_fmt_into(dst, len, offset, indent, limit, as_vector_symbol(keys)[i]);
            break;
        default:
            n = rf_object_fmt_into(dst, len, offset, indent, limit, &as_list(keys)[i]);
            break;
        }

        chk(n);

        n = str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, ": ");

        chk(n);

        // Dispatch rf_objects type
        switch (vals->type)
        {
        case TYPE_I64:
            n = i64_fmt_into(dst, len, offset, 0, limit, as_vector_i64(vals)[i]);
            break;
        case TYPE_F64:
            n = f64_fmt_into(dst, len, offset, 0, limit, as_vector_f64(vals)[i]);
            break;
        case TYPE_SYMBOL:
            n = symbol_fmt_into(dst, len, offset, 0, limit, as_vector_symbol(vals)[i]);
            break;
        default:
            n = rf_object_fmt_into(dst, len, offset, 0, limit, &as_list(vals)[i]);
            break;
        }

        chk(n);

        n = str_fmt_into(dst, len, offset, MAX_ROW_WIDTH, "\n");

        chk(n);
    }

    indent -= 2;
    return str_fmt_into(dst, len, offset, limit, "%*.*s}", indent, indent, PADDING);
}

i32_t table_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    return 0;
    // i64_t *header = as_vector_symbol(&as_list(rf_object)[0]);
    // rf_object_t *columns = &as_list(rf_object)[1], column_widths;
    // i32_t table_width, width, table_height;
    // str_t str = str_fmt(0, "|"), s;
    // str_t formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL}};
    // i32_t offset = 1, i, j;

    // table_width = (&as_list(rf_object)[0])->adt->len;
    // if (table_width > TABLE_MAX_WIDTH)
    //     table_width = TABLE_MAX_WIDTH;

    // table_height = (&as_list(columns)[0])->adt->len;
    // if (table_height > TABLE_MAX_HEIGHT)
    //     table_height = TABLE_MAX_HEIGHT;

    // column_widths = vector_i64(table_width);

    // // Calculate each column maximum width
    // for (i = 0; i < table_width; i++)
    // {
    //     // First check the column name
    //     width = strlen(symbols_get(header[i]));
    //     as_vector_i64(&column_widths)[i] = width;

    //     // Then traverse column until maximum height limit
    //     for (j = 0; j < table_height; j++)
    //     {
    //         rf_object_t *column = &as_list(columns)[i];

    //         switch (column->type)
    //         {
    //         case TYPE_I64:
    //             s = i64_fmt_into(limit, as_vector_i64(column)[j]);
    //             break;
    //         case TYPE_F64:
    //             s = f64_fmt_into(limit, as_vector_f64(column)[j]);
    //             break;
    //         case TYPE_SYMBOL:
    //             s = str_fmt(limit, "%s", symbols_get(as_vector_symbol(column)[j]));
    //             break;
    //         default:
    //             s = rf_object_fmt_indent(indent, limit - indent, &as_list(column)[j]);
    //             break;
    //         }

    //         formatted_columns[i][j] = s;
    //         width = strlen(s);
    //         if (width > as_vector_i64(&column_widths)[i])
    //             as_vector_i64(&column_widths)[i] = width;
    //     }
    // }

    // // Print table header
    // for (i = 0; i < table_width; i++)
    // {
    //     width = as_vector_i64(&column_widths)[i];
    //     s = symbols_get(header[i]);
    //     width = width - strlen(s);
    //     offset += str_fmt_into(0, offset, &str, " %s%*.*s |", s, width, width, PADDING);
    // }

    // // Print table header separator
    // offset += str_fmt_into(0, offset, &str, "\n+");

    // for (i = 0; i < table_width; i++)
    // {
    //     width = as_vector_i64(&column_widths)[i] + 2;
    //     offset += str_fmt_into(0, offset, &str, "%*.*s+", width, width, TABLE_HEADER_SEPARATOR);
    // }

    // // Print table content
    // for (j = 0; j < table_height; j++)
    // {
    //     offset += str_fmt_into(0, offset, &str, "\n|");

    //     for (i = 0; i < table_width; i++)
    //     {
    //         width = as_vector_i64(&column_widths)[i] + 1;
    //         s = formatted_columns[i][j];
    //         offset += str_fmt_into(0, offset, &str, " %s%*.*s|", s, width - strlen(s), width - strlen(s), PADDING);
    //         // Free formatted column
    //         rf_free(s);
    //     }
    // }

    // return str;
}

i32_t error_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *error)
{
    UNUSED(limit);
    return str_fmt_into(dst, len, offset, 0, "** [E%.3d] error: %s", error->adt->code, as_string(error));
}

extern i32_t rf_object_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t indent, i32_t limit, rf_object_t *rf_object)
{
    switch (rf_object->type)
    {
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
    case TYPE_ERROR:
        return error_fmt_into(dst, len, offset, indent, limit, rf_object);
    default:
        return str_fmt_into(dst, len, offset, limit, "null");
    }
}

extern str_t rf_object_fmt(rf_object_t *rf_object)
{
    i32_t len = 0, offset = 0, limit = MAX_ROW_WIDTH;
    str_t dst = NULL;
    rf_object_fmt_into(&dst, &len, &offset, 0, limit, rf_object);
    if (dst == NULL)
        panic("format: returns null");

    return dst;
}

extern str_t type_fmt(i8_t type)
{
    return str_fmt(0, 0, "%s", symbols_get(env_get_typename_by_type(&runtime_get()->env, type)));
}
