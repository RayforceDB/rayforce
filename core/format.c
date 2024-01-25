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
#include "error.h"

#define MAX_ROW_WIDTH 80
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 2
#define TABLE_MAX_WIDTH 10      // Maximum number of columns
#define TABLE_MAX_HEIGHT 20     // Maximum number of rows
#define LIST_MAX_HEIGHT 5       // Maximum number of list/dict rows
#define ERR_STACK_MAX_HEIGHT 10 // Maximum number of error stack frames
#define MAX_ERROR_LEN 4096

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
i32_t str_vfmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, str_t fmt, va_list vargs)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p, s;
    va_list args;

    // If ptr is NULL, realloc behaves like malloc.
    if (*len <= (size + *offset))
    {
        *len += size + 1;
        s = heap_realloc(*dst, *len);

        if (s == NULL)
        {
            heap_free(*dst);
            panic("str_vfmt_into: OOM");
        }

        *dst = s;
    }

    while (1)
    {
        p = *dst + *offset;

        va_copy(args, vargs); // Make a copy of args to use with vsnprintf
        n = vsnprintf(p, size, fmt, args);
        va_end(vargs);

        if (n < 0)
        {
            if (*dst != NULL)
                heap_free(*dst);

            panic("str_vfmt_into: OOM");
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
            panic("str_vfmt_into: OOM");
        }

        *dst = s;
        *len += size;
    }

    *offset += n;

    return n;
}

i32_t str_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, str_t fmt, ...)
{
    i32_t n;
    va_list args;

    va_start(args, fmt);
    n = str_vfmt_into(dst, len, offset, limit, fmt, args);
    va_end(args);

    return n;
}

str_t str_vfmt(i32_t limit, str_t fmt, va_list vargs)
{
    i32_t n = 0, size = limit > 0 ? limit : MAX_ROW_WIDTH;
    str_t p = heap_alloc(size), s;
    va_list args;

    if (!p)
    {
        // Handle allocation failure
        return NULL;
    }

    while (1)
    {
        va_copy(args, vargs); // Make a copy of args to use with vsnprintf
        n = vsnprintf(p, size, fmt, args);
        va_end(vargs);

        if (n < 0)
        {
            // Handle encoding error
            heap_free(p);
            return NULL;
        }

        if (n < size)
            break; // Buffer is large enough

        if (limit > 0)
            return p; // We have a limit and it's reached

        size = n + 1; // Increase buffer size
        s = heap_realloc(p, size);
        if (!s)
        {
            // Handle reallocation failure
            heap_free(p);
            return NULL;
        }
        p = s;
    }

    return p;
}

str_t str_fmt(i32_t limit, str_t fmt, ...)
{
    str_t res;
    va_list args;

    va_start(args, fmt);
    res = str_vfmt(limit, fmt, args);
    va_end(args);

    return res;
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

i32_t char_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, bool_t full, char_t val)
{
    switch (val)
    {
    case '\"':
        return full ? str_fmt_into(dst, len, offset, limit, "'\\\"'") : str_fmt_into(dst, len, offset, limit, "\\\"");
    case '\n':
        return full ? str_fmt_into(dst, len, offset, limit, "'\\n'") : str_fmt_into(dst, len, offset, limit, "\\n");
    case '\r':
        return full ? str_fmt_into(dst, len, offset, limit, "'\\r'") : str_fmt_into(dst, len, offset, limit, "\\r");
    case '\t':
        return full ? str_fmt_into(dst, len, offset, limit, "'\\t'") : str_fmt_into(dst, len, offset, limit, "\\t");
    default:
        return full ? str_fmt_into(dst, len, offset, limit, "'%c'", val) : str_fmt_into(dst, len, offset, limit, "%c", val);
    }
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

    if (ops_is_nan(val))
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
    i32_t n;

    if (!ts.hours && !ts.mins && !ts.secs && !ts.nanos)
        n = str_fmt_into(dst, len, offset, limit, "%.4d.%.2d.%.2d", ts.year, ts.month, ts.day);
    else
        n = str_fmt_into(dst, len, offset, limit, "%.4d.%.2d.%.2dD%.2d:%.2d:%.2d.%.9d",
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

i32_t error_frame_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj, i64_t idx, str_t msg)
{
    unused(limit);
    i32_t n = 0;
    u32_t line_len;
    u16_t line_number = 0, i;
    str_t filename, source, function, start, end, lf = "";
    obj_t *frame = as_list(obj);
    span_t span = (span_t){0};

    span.id = frame[0]->i64;
    filename = frame[1] ? as_string(frame[1]) : "repl";
    function = frame[2] ? symtostr(frame[2]->i64) : "anonymous";
    source = as_string(frame[3]);

    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s [%lld] %s%s-->%s %s:%d:%d..%d:%d in function: @%s\n",
                      MAGENTA, idx, RESET, CYAN, RESET, filename,
                      span.start_line + 1, span.start_column + 1, span.end_line + 1, span.end_column + 1,
                      function);

    start = source;
    end = NULL;

    while (1)
    {
        end = strchr(start, '\n');

        if (end == NULL)
        {
            end = source + strlen(source) - 1;
            lf = "\n";
        }

        line_len = end - start + 1;

        if (line_number >= span.start_line && line_number <= span.end_line)
        {
            n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "  %.3d %s|%s %.*s", line_number + 1, CYAN, RESET, line_len, start);

            if (span.start_line == span.end_line)
            {
                n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s      %s:%s ", lf, CYAN, RESET);
                for (i = 0; i < span.start_column; i++)
                    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, " ");

                for (i = span.start_column; i < span.end_column; i++)
                    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s~%s", TOMATO, RESET);

                n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s~ %s%s\n", TOMATO, msg, RESET);
            }
            else
            {
                if (line_number == span.start_line)
                {
                    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "      %s:%s ", CYAN, RESET);
                    for (i = 0; i < span.start_column; i++)
                        n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, " ");

                    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s~%s\n", TOMATO, RESET);
                }
                else if (line_number == span.end_line)
                {
                    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s      :%s", CYAN, RESET);
                    for (i = 0; i < span.end_column + 1; i++)
                        n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, " ");

                    n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s~ %s%s\n", TOMATO, msg, RESET);
                }
            }
        }

        if (line_number > span.end_line)
            break;

        line_number++;
        start = end + 1;
    }

    return n;
}

i32_t error_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj)
{
    i32_t n = 0;
    u16_t i, l, m;
    str_t error_desc, msg;
    error_t *error = as_error(obj);

    switch (error->code)
    {
    case ERR_INIT:
        error_desc = "unable to initialize";
        break;
    case ERR_PARSE:
        error_desc = "unable to parse input";
        break;
    case ERR_EVAL:
        error_desc = "object evaluation failed";
        break;
    case ERR_FORMAT:
        error_desc = "unable to format object";
        break;
    case ERR_TYPE:
        error_desc = "type mismatch";
        break;
    case ERR_LENGTH:
        error_desc = "length mismatch";
        break;
    case ERR_ARITY:
        error_desc = "arity mismatch";
        break;
    case ERR_INDEX:
        error_desc = "index out of bounds";
        break;
    case ERR_HEAP:
        error_desc = "heap allocation failed";
        break;
    case ERR_IO:
        error_desc = "io";
        break;
    case ERR_SYS:
        error_desc = "system";
        break;
    case ERR_NOT_FOUND:
        error_desc = "object not found";
        break;
    case ERR_NOT_EXIST:
        error_desc = "object does not exist";
        break;
    case ERR_NOT_IMPLEMENTED:
        error_desc = "not implemented";
        break;
    case ERR_STACK_OVERFLOW:
        error_desc = "stack overflow";
        break;
    case ERR_RAISE:
        error_desc = "raised error";
        break;
    default:
        error_desc = "corrupted error object/unknown error";
    }

    msg = as_string(error->msg);

    // there is a locations
    if (error->locs)
    {
        n += str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "%s** [E%.3lld] error%s: %s\n", TOMATO, error->code, RESET, error_desc);

        l = error->locs->len;
        m = l > ERR_STACK_MAX_HEIGHT ? ERR_STACK_MAX_HEIGHT : l;
        for (i = 0; i < m; i++)
        {
            n += error_frame_fmt_into(dst, len, offset, MAX_ERROR_LEN, as_list(error->locs)[i], l - i - 1, msg);
            msg = "";
        }

        if (l > m)
        {
            n += str_fmt_into(dst, len, offset, limit, "  ..\n");
            n += error_frame_fmt_into(dst, len, offset, MAX_ERROR_LEN, as_list(error->locs)[l - 1], 0, msg);
        }

        return n;
    }

    return str_fmt_into(dst, len, offset, MAX_ERROR_LEN, "** [E%.3lld] error: %s: %s", error->code, error_desc, msg);
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
        return char_fmt_into(dst, len, offset, limit, true, as_string(obj)[i]);
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
            maxn(n, obj_fmt_into(dst, len, offset, indent, limit, false, as_list(obj)[i]));
            maxn(n, str_fmt_into(dst, len, offset, 0, " "));
        }

        maxn(n, obj_fmt_into(dst, len, offset, indent, limit, false, as_list(obj)[i]));

        if (list_height < (i32_t)obj->len)
            maxn(n, str_fmt_into(dst, len, offset, 0, ".."));

        maxn(n, str_fmt_into(dst, len, offset, 0, ")"));

        return n;
    }

    indent += 2;

    for (i = 0; i < list_height; i++)
    {
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s", indent, indent, PADDING));
        maxn(n, obj_fmt_into(dst, len, offset, indent, limit, false, as_list(obj)[i]));
    }

    if (list_height < (i32_t)obj->len)
        maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s..", indent, indent, PADDING));

    indent -= 2;

    maxn(n, str_fmt_into(dst, len, offset, 0, "\n%*.*s)", indent, indent, PADDING));

    return n;
}

i32_t string_fmt_into(str_t *dst, i32_t *len, i32_t *offset, i32_t limit, obj_t obj)
{
    i32_t n;
    u64_t i, l;

    n = str_fmt_into(dst, len, offset, limit, "\"");

    l = obj->len - 1; // skip trailing \0
    for (i = 0; i < l; i++)
    {
        n += char_fmt_into(dst, len, offset, limit, false, as_string(obj)[i]);
        if (n > limit)
            break;
    }

    if (n > limit)
        n += str_fmt_into(dst, len, offset, 3, "..");

    n += str_fmt_into(dst, len, offset, 2, "\"");

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
    i32_t n;
    u64_t i, dict_height = ops_count(keys);

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

        if (dict_height < ops_count(keys))
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

    if (dict_height < ops_count(keys))
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

    table_width = (as_list(obj)[0])->len;
    if (table_width == 0)
        return str_fmt_into(dst, len, offset, 0, "@table");

    n = str_fmt_into(dst, len, offset, 0, "|");

    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    table_height = ops_count(obj);
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

    if (as_lambda(obj)->name)
        n = str_fmt_into(dst, len, offset, indent, "@%s", symtostr((as_lambda(obj)->name)->i64));
    else
    {
        n = str_fmt_into(dst, len, offset, indent, "(fn ");
        n += obj_fmt_into(dst, len, offset, indent, limit, false, as_lambda(obj)->args);
        n += str_fmt_into(dst, len, offset, indent, " ");
        n += obj_fmt_into(dst, len, offset, indent, limit, false, as_lambda(obj)->body);
        n += str_fmt_into(dst, len, offset, indent, ")");
    }

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
 * Format an obj into a rayforce string
 */
obj_t obj_stringify(obj_t obj)
{
    obj_t res = string(0);
    str_t dst = (str_t)res;
    i32_t n, len = sizeof(struct obj_t),
             offset = sizeof(struct obj_t), limit = MAX_ROW_WIDTH;

    if (obj == NULL)
        n = str_fmt_into(&dst, &len, &offset, limit, "null");
    else
        n = obj_fmt_into(&dst, &len, &offset, 0, limit, true, obj);

    if (dst == NULL)
        panic("format: returns null");

    res = (obj_t)dst;
    res->len = n + 1; // + 1 for '\0'

    return res;
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
        panic("format: returns null");

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
