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
#include "filter.h"

#define MAX_ROW_WIDTH 800
#define FORMAT_TRAILER_SIZE 4
#define F64_PRECISION 2
#define TABLE_MAX_WIDTH 10      // Maximum number of columns
#define TABLE_MAX_HEIGHT 20     // Maximum number of rows
#define LIST_MAX_HEIGHT 5       // Maximum number of list/dict rows
#define ERR_STACK_MAX_HEIGHT 10 // Maximum number of error stack frames
#define MAX_ERROR_LEN 4096
#define NO_LIMIT -1

#define maxn(n, e)         \
    {                      \
        i64_t k = e;       \
        n = n > k ? n : k; \
    }

nil_t debug_str(obj_p str)
{
    u64_t i, l;
    str_p s;

    l = str->len;
    s = as_string(str);

    printf("** s: ");
    for (i = 0; i < l; i++)
        printf("0x%hhx ", s[i]);

    printf("\n");
}

/*
 * return n:
 * n > limit - truncated
 * n < limit - fits into buffer
 * n < 0 - error
 */
i64_t str_vfmt_into(obj_p *dst, i64_t limit, str_p fmt, va_list vargs)
{
    str_p s;
    i64_t n = 0, l, o, size;
    va_list args;

    if (limit == 0)
        return 0;

    size = 2; // start from 2 symbols: 1 for the string and 1 for the null terminator

    // Allocate or expand the buffer if necessary
    if (*dst == NULL_OBJ)
    {
        *dst = string(size);
        if (*dst == NULL_OBJ)
            panic("str_vfmt_into: OOM");

        l = 0;
    }
    else
    {
        l = (*dst)->len;
        if (is_null(resize_obj(dst, l + size)))
        {
            heap_free(*dst);
            panic("str_vfmt_into: OOM");
        }
    }

    o = l;

    while (B8_TRUE)
    {
        s = as_string(*dst) + o;
        va_copy(args, vargs); // Make a copy of args to use with vsnprintf
        n = vsnprintf(s, size, fmt, args);
        va_end(args); // args should be ended, not vargs

        if (n < 0)
        {
            heap_free(*dst);
            panic("str_vfmt_into: Error in vsnprintf");
        }

        if (n < size)
        {
            resize_obj(dst, l + n); // resize the buffer to fit the string
            return n;               // n fits into buffer, return n
        }

        // n >= size, the buffer is too small, but we reached the limit
        if (limit != NO_LIMIT && n >= limit)
            return limit;

        // Expand the buffer for the next iteration
        size = n + 1; // +1 for the null terminator
        if (is_null(resize_obj(dst, l + size)))
        {
            heap_free(*dst);
            panic("str_vfmt_into: OOM");
        }
    }
}

i64_t str_fmt_into(obj_p *dst, i64_t limit, str_p fmt, ...)
{
    i64_t n;
    va_list args;

    va_start(args, fmt);
    n = str_vfmt_into(dst, limit, fmt, args);
    va_end(args);

    return n;
}

i64_t str_fmt_into_n(obj_p *dst, i64_t limit, i64_t repeat, str_p fmt, ...)
{
    i64_t i, n;

    for (i = 0, n = 0; i < repeat; i++)
        n += str_fmt_into(dst, limit, fmt);

    return n;
}

obj_p str_vfmt(i64_t limit, str_p fmt, va_list vargs)
{
    i64_t n = 0, size;
    obj_p res;
    str_p s;
    va_list args;

    if (limit == 0)
        return NULL_OBJ;

    size = 2;

    res = string(size);

    if (is_null(res))
        panic("str_vfmt: OOM");

    while (B8_TRUE)
    {
        s = as_string(res);
        va_copy(args, vargs); // Make a copy of args to use with vsnprintf
        n = vsnprintf(s, size, fmt, args);
        va_end(vargs);

        if (n < 0)
        {
            // Handle encoding error
            heap_free(res);
            return NULL_OBJ;
        }

        if (n < size)
            break; // Buffer is large enough

        if (limit > 0)
            return res; // We have a limit and it's reached

        size = n + 1; // Increase buffer size
        res = resize_obj(&res, size);

        if (is_null(res))
            panic("str_vfmt: OOM");
    }

    return res;
}

obj_p str_fmt(i64_t limit, str_p fmt, ...)
{
    obj_p res;
    va_list args;

    va_start(args, fmt);
    res = str_vfmt(limit, fmt, args);
    va_end(args);

    return res;
}

i64_t b8_fmt_into(obj_p *dst, b8_t val)
{
    if (val)
        return str_fmt_into(dst, 5, "true");

    return str_fmt_into(dst, 6, "false");
}

i64_t byte_fmt_into(obj_p *dst, u8_t val)
{
    return str_fmt_into(dst, 5, "0x%02x", val);
}

i64_t char_fmt_into(obj_p *dst, b8_t full, c8_t val)
{
    switch (val)
    {
    case '\"':
        return full ? str_fmt_into(dst, 5, "'\\\"'") : str_fmt_into(dst, 4, "\\\"");
    case '\n':
        return full ? str_fmt_into(dst, 5, "'\\n'") : str_fmt_into(dst, 3, "\\n");
    case '\r':
        return full ? str_fmt_into(dst, 5, "'\\r'") : str_fmt_into(dst, 3, "\\r");
    case '\t':
        return full ? str_fmt_into(dst, 5, "'\\t'") : str_fmt_into(dst, 3, "\\t");
    default:
        return full ? str_fmt_into(dst, 4, "'%c'", val) : str_fmt_into(dst, 2, "%c", val);
    }
}

i64_t i64_fmt_into(obj_p *dst, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, 3, "%s", "0i");

    return str_fmt_into(dst, NO_LIMIT, "%lld", val);
}

i64_t f64_fmt_into(obj_p *dst, f64_t val)
{
    f64_t order;

    if (ops_is_nan(val))
        return str_fmt_into(dst, 3, "0f");

    // Find the order of magnitude of the number to select the appropriate format
    order = log10(val);
    if (val && (order > 6 || order < -4))
        return str_fmt_into(dst, NO_LIMIT, "%.*e", 3 * F64_PRECISION, val);

    return str_fmt_into(dst, NO_LIMIT, "%.*f", F64_PRECISION, val);
}

i64_t ts_fmt_into(obj_p *dst, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, 3, "0t");

    timestamp_t ts = timestamp_from_i64(val);
    i64_t n;

    if (!ts.hours && !ts.mins && !ts.secs && !ts.nanos)
        n = str_fmt_into(dst, 11, "%.4d.%.2d.%.2d", ts.year, ts.month, ts.day);
    else
        n = str_fmt_into(dst, 30, "%.4d.%.2d.%.2dD%.2d:%.2d:%.2d.%.9d",
                         ts.year, ts.month, ts.day, ts.hours, ts.mins, ts.secs, ts.nanos);

    return n;
}

i64_t guid_fmt_into(obj_p *dst, guid_t *val)
{
    u8_t *guid = val->buf, NULL_GUID[16] = {0};

    if (memcmp(guid, NULL_GUID, 16) == 0)
        return str_fmt_into(dst, 3, "0g");

    i64_t n = str_fmt_into(dst, 37, "%02hhx%02hhx%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                           guid[0], guid[1], guid[2], guid[3],
                           guid[4], guid[5], guid[6], guid[7],
                           guid[8], guid[9], guid[10], guid[11],
                           guid[12], guid[13], guid[14], guid[15]);

    return n;
}

i64_t symbol_fmt_into(obj_p *dst, i64_t limit, i64_t val)
{
    if (val == NULL_I64)
        return str_fmt_into(dst, 3, "0s");

    i64_t n = str_fmt_into(dst, limit, "%s", strof_sym(val));

    if (n > limit)
        n += str_fmt_into(dst, 3, "..");

    return n;
}

i64_t string_fmt_into(obj_p *dst, i64_t limit, obj_p obj)
{
    i64_t n;
    u64_t i, l;
    str_p s;

    n = str_fmt_into(dst, 2, "\"");
    l = obj->len;
    s = as_string(obj);

    for (i = 0; i < l; i++)
    {
        n += char_fmt_into(dst, B8_FALSE, s[i]);
        if (n > limit)
            break;
    }

    if (n > limit)
        n += str_fmt_into(dst, 3, "..");

    n += str_fmt_into(dst, 2, "\"");

    return n;
}

i64_t error_frame_fmt_into(obj_p *dst, i64_t limit, obj_p obj, i64_t idx, str_p msg, u64_t msg_len)
{
    unused(limit);
    i64_t n = 0;
    u32_t line_len, fname_len;
    u16_t line_number = 0, i;
    str_p filename, source, function, start, end, lf = "", flname = "repl", fnname = "anonymous";
    obj_p *frame = as_list(obj);
    span_t span = (span_t){0};

    span.id = frame[0]->i64;
    if (frame[1] != NULL_OBJ)
    {
        filename = as_string(frame[1]);
        fname_len = frame[1]->len;
    }
    else
    {
        filename = flname;
        fname_len = strlen(flname);
    }
    function = (frame[2] != NULL_OBJ) ? strof_sym(frame[2]->i64) : fnname;
    source = as_string(frame[3]);
    line_len = frame[3]->len;

    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s [%lld] %s%s-->%s %.*s:%d:%d..%d:%d in function: @%s\n",
                      MAGENTA, idx, RESET, CYAN, RESET, fname_len, filename,
                      span.start_line + 1, span.start_column + 1, span.end_line + 1, span.end_column + 1,
                      function);

    start = source;
    end = NULL;

    while (1)
    {
        end = memchr(start, '\n', line_len);

        if (end == NULL)
        {
            end = source + strlen(source) - 1;
            lf = "\n";
        }

        line_len = end - start + 1;

        if (line_number >= span.start_line && line_number <= span.end_line)
        {
            n += str_fmt_into(dst, MAX_ERROR_LEN, "  %.3d %s|%s %.*s", line_number + 1, CYAN, RESET, line_len, start);

            if (span.start_line == span.end_line)
            {
                n += str_fmt_into(dst, MAX_ERROR_LEN, "%s      %s:%s ", lf, CYAN, RESET);
                for (i = 0; i < span.start_column; i++)
                    n += str_fmt_into(dst, MAX_ERROR_LEN, " ");

                for (i = span.start_column; i < span.end_column; i++)
                    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s~%s", TOMATO, RESET);

                n += str_fmt_into(dst, MAX_ERROR_LEN, "%s~ %.*s%s\n", TOMATO, msg_len, msg, RESET);
            }
            else
            {
                if (line_number == span.start_line)
                {
                    n += str_fmt_into(dst, MAX_ERROR_LEN, "      %s:%s ", CYAN, RESET);
                    for (i = 0; i < span.start_column; i++)
                        n += str_fmt_into(dst, MAX_ERROR_LEN, " ");

                    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s~%s\n", TOMATO, RESET);
                }
                else if (line_number == span.end_line)
                {
                    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s%s      :%s", lf, CYAN, RESET);
                    for (i = 0; i < span.end_column + 1; i++)
                        n += str_fmt_into(dst, MAX_ERROR_LEN, " ");

                    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s~ %.*s%s\n", TOMATO, msg_len, msg, RESET);
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

i64_t error_fmt_into(obj_p *dst, i64_t limit, obj_p obj)
{
    i64_t n = 0;
    u64_t msg_len;
    u16_t i, l, m;
    str_p error_desc, msg;
    ray_error_p error = as_error(obj);

    switch (error->code)
    {
    case ERR_INIT:
        error_desc = "initialize error";
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

    msg_len = error->msg->len;
    msg = as_string(error->msg);

    // there is a locations
    if (error->locs != NULL_OBJ)
    {
        n += str_fmt_into(dst, MAX_ERROR_LEN, "%s** [E%.3lld] error%s: %s\n", TOMATO, error->code, RESET, error_desc);
        l = error->locs->len;
        m = l > ERR_STACK_MAX_HEIGHT ? ERR_STACK_MAX_HEIGHT : l;

        for (i = 0; i < m; i++)
        {
            n += error_frame_fmt_into(dst, MAX_ERROR_LEN, as_list(error->locs)[i], l - i - 1, msg, msg_len);
            msg = "";
            msg_len = 0;
        }

        if (l > m)
        {
            n += str_fmt_into(dst, limit, "  ..\n");
            n += error_frame_fmt_into(dst, MAX_ERROR_LEN, as_list(error->locs)[l - 1], 0, msg, msg_len);
        }

        return n;
    }

    return str_fmt_into(dst, MAX_ERROR_LEN, "%s** [E%.3lld] error%s: %s: %.*s", TOMATO, error->code, RESET,
                        error_desc, msg_len, msg);
}

i64_t raw_fmt_into(obj_p *dst, i64_t indent, i64_t limit, obj_p obj, i64_t i)
{
    obj_p idx, res;
    i64_t n;

    switch (obj->type)
    {
    case TYPE_B8:
        return b8_fmt_into(dst, as_b8(obj)[i]);
    case TYPE_U8:
        return byte_fmt_into(dst, as_u8(obj)[i]);
    case TYPE_I64:
        return i64_fmt_into(dst, as_i64(obj)[i]);
    case TYPE_F64:
        return f64_fmt_into(dst, as_f64(obj)[i]);
    case TYPE_SYMBOL:
        return symbol_fmt_into(dst, limit, as_symbol(obj)[i]);
    case TYPE_TIMESTAMP:
        return ts_fmt_into(dst, as_timestamp(obj)[i]);
    case TYPE_GUID:
        return guid_fmt_into(dst, &as_guid(obj)[i]);
    case TYPE_C8:
        return char_fmt_into(dst, B8_TRUE, as_string(obj)[i]);
    case TYPE_LIST:
        return obj_fmt_into(dst, indent, limit, B8_FALSE, as_list(obj)[i]);
    case TYPE_ENUM:
    case TYPE_ANYMAP:
        idx = i64(i);
        res = ray_at(obj, idx);
        drop_obj(idx);
        if (is_error(res))
        {
            drop_obj(res);
            return error_fmt_into(dst, limit, res);
        }
        n = obj_fmt_into(dst, indent, limit, B8_FALSE, res);
        drop_obj(res);
        return n;
    default:
        return str_fmt_into(dst, limit, "null");
    }
}

i64_t vector_fmt_into(obj_p *dst, i64_t limit, obj_p obj)
{
    if (obj->len == 0)
        return str_fmt_into(dst, limit, "[]");

    i64_t i, n = str_fmt_into(dst, limit, "["), indent = 0;
    i64_t l;

    if (n > limit)
        return n;

    l = obj->len;

    for (i = 0; i < l; i++)
    {
        n += raw_fmt_into(dst, indent, limit, obj, i);
        if (n > limit)
            break;

        if (i + 1 < l)
            n += str_fmt_into(dst, 2, " ");

        if (n > limit)
            break;
    }

    if (n > limit)
        n += str_fmt_into(dst, 4, "..]");
    else
        n += str_fmt_into(dst, 2, "]");

    return n;
}

i64_t list_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj)
{
    i64_t i, n, list_height = obj->len;

    if (list_height == 0)
        return str_fmt_into(dst, 3, "()");

    n = str_fmt_into(dst, 2, "(");

    if (list_height > LIST_MAX_HEIGHT)
        list_height = LIST_MAX_HEIGHT;

    if (!full)
    {
        for (i = 0; i < list_height - 1; i++)
        {
            maxn(n, obj_fmt_into(dst, indent, limit, B8_FALSE, as_list(obj)[i]));
            maxn(n, str_fmt_into(dst, 2, " "));
        }

        maxn(n, obj_fmt_into(dst, indent, limit, B8_FALSE, as_list(obj)[i]));

        if (list_height < (i64_t)obj->len)
            maxn(n, str_fmt_into(dst, 3, ".."));

        maxn(n, str_fmt_into(dst, 2, ")"));

        return n;
    }

    indent += 2;

    for (i = 0; i < list_height; i++)
    {
        maxn(n, str_fmt_into(dst, 2, "\n"));
        maxn(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
        maxn(n, obj_fmt_into(dst, indent, limit, B8_FALSE, as_list(obj)[i]));
    }

    if (list_height < (i64_t)obj->len)
    {
        maxn(n, str_fmt_into(dst, 2, "\n"));
        maxn(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
        maxn(n, str_fmt_into(dst, 3, ".."));
    }

    indent -= 2;

    maxn(n, str_fmt_into(dst, 2, "\n"));
    maxn(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
    maxn(n, str_fmt_into(dst, 2, ")"));

    return n;
}

i64_t enum_fmt_into(obj_p *dst, i64_t indent, i64_t limit, obj_p obj)
{
    i64_t n;
    obj_p s, e, idx;

    s = ray_key(obj);
    if (enum_val(obj)->len >= TABLE_MAX_HEIGHT)
    {
        limit = TABLE_MAX_HEIGHT;
        idx = i64(TABLE_MAX_HEIGHT);
        e = ray_take(idx, obj);
        drop_obj(idx);
    }
    else
        e = ray_value(obj);

    n = str_fmt_into(dst, 2, "'");
    n += obj_fmt_into(dst, indent, limit, B8_FALSE, s);
    n += str_fmt_into(dst, 2, "#");
    n += obj_fmt_into(dst, indent, limit, B8_FALSE, e);

    drop_obj(s);
    drop_obj(e);

    return n;
}

i64_t anymap_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj)
{
    i64_t n;
    obj_p a, idx;

    if (anymap_val(obj)->len >= TABLE_MAX_HEIGHT)
    {
        limit = TABLE_MAX_HEIGHT;
        idx = i64(TABLE_MAX_HEIGHT);
        a = ray_take(idx, obj);
        drop_obj(idx);
    }
    else
        a = ray_value(obj);

    n = obj_fmt_into(dst, indent, limit, full, a);

    drop_obj(a);

    return n;
}

i64_t dict_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj)
{
    obj_p keys = as_list(obj)[0], vals = as_list(obj)[1];
    i64_t n;
    u64_t i, dict_height = ops_count(keys);

    if (dict_height == 0)
        return str_fmt_into(dst, 3, "{}");

    n = str_fmt_into(dst, 2, "{");

    if (dict_height > LIST_MAX_HEIGHT)
        dict_height = LIST_MAX_HEIGHT;

    if (!full)
    {
        for (i = 0; i < dict_height - 1; i++)
        {
            maxn(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, keys, i));
            maxn(n, str_fmt_into(dst, MAX_ROW_WIDTH, ": "));
            maxn(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, vals, i));
            maxn(n, str_fmt_into(dst, MAX_ROW_WIDTH, " "));
        }

        maxn(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, keys, i));
        maxn(n, str_fmt_into(dst, MAX_ROW_WIDTH, ": "));
        maxn(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, vals, i));

        if (dict_height < ops_count(keys))
            maxn(n, str_fmt_into(dst, 3, ".."));

        maxn(n, str_fmt_into(dst, 2, "}"));

        return n;
    }

    indent += 2;

    for (i = 0; i < dict_height; i++)
    {
        maxn(n, str_fmt_into(dst, 2, "\n"));
        maxn(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
        maxn(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, keys, i));
        n += str_fmt_into(dst, MAX_ROW_WIDTH, ": ");
        maxn(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, vals, i));
    }

    if (dict_height < ops_count(keys))
    {
        maxn(n, str_fmt_into(dst, 2, "\n"));
        maxn(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
        maxn(n, str_fmt_into(dst, 3, ".."));
    }

    indent -= 2;

    maxn(n, str_fmt_into(dst, 2, "\n"));
    maxn(n, str_fmt_into_n(dst, limit, indent, " "));
    maxn(n, str_fmt_into(dst, 2, "}"));

    return n;
}

i64_t table_fmt_into(obj_p *dst, i64_t indent, b8_t full, obj_p obj)
{
    i64_t *header = as_symbol(as_list(obj)[0]);
    obj_p s, column, columns = as_list(obj)[1], column_widths,
                     formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL_OBJ}};
    str_p p;
    i64_t i, j, n, m, table_width, table_height;

    if (!full)
    {
        n = str_fmt_into(dst, 8, "(table ");
        n += obj_fmt_into(dst, indent, MAX_ROW_WIDTH, B8_FALSE, as_list(obj)[0]);
        n += str_fmt_into(dst, 5, " ..)");

        return n;
    }

    table_width = (as_list(obj)[0])->len;
    if (table_width == 0)
        return str_fmt_into(dst, 7, "@table");

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
        n = strlen(strof_sym(header[i]));

        // Then traverse column until maximum height limit
        for (j = 0; j < table_height; j++)
        {
            column = as_list(columns)[i];
            s = NULL_OBJ;
            m = raw_fmt_into(&s, 0, 38, column, j);
            formatted_columns[i][j] = s;
            maxn(n, m);
        }

        as_i64(column_widths)[i] = n + 2; // Add 2 for padding
    }

    n = str_fmt_into(dst, 2, "|");

    // Print table header
    for (i = 0; i < table_width; i++)
    {
        n = as_i64(column_widths)[i];
        p = strof_sym(header[i]);
        n = n - strlen(p) - 1;
        str_fmt_into(dst, NO_LIMIT, " %s", p);
        str_fmt_into_n(dst, NO_LIMIT, n, " ");
        str_fmt_into(dst, 2, "|");
    }

    if (as_list(obj)[0]->len > TABLE_MAX_WIDTH)
        str_fmt_into(dst, 4, " ..");

    // Print table header separator
    str_fmt_into(dst, 2, "\n");
    str_fmt_into_n(dst, NO_LIMIT, indent, " ");
    str_fmt_into(dst, 2, "+");

    for (i = 0; i < table_width; i++)
    {
        n = as_i64(column_widths)[i];
        for (j = 0; j < n; j++)
            str_fmt_into(dst, 2, "-");

        str_fmt_into(dst, 2, "+");
    }

    // Print table content
    for (j = 0; j < table_height; j++)
    {
        str_fmt_into(dst, 2, "\n");
        str_fmt_into_n(dst, NO_LIMIT, indent, " ");
        str_fmt_into(dst, 2, "|");

        for (i = 0; i < table_width; i++)
        {
            n = as_i64(column_widths)[i] - 1;
            s = formatted_columns[i][j];
            p = as_string(s);
            n = n - s->len;
            str_fmt_into(dst, s->len + 2, " %.*s", s->len, p);
            str_fmt_into_n(dst, NO_LIMIT, n, " ");
            str_fmt_into(dst, 2, "|");
            // Free formatted column
            drop_obj(s);
        }
    }

    drop_obj(column_widths);

    if ((table_height > 0) && ((u64_t)table_height < as_list(columns)[0]->len))
        str_fmt_into(dst, 4, "\n..");

    return n;
}

i64_t internal_fmt_into(obj_p *dst, i64_t limit, obj_p obj)
{
    return str_fmt_into(dst, limit, "%s", env_get_internal_name(obj));
}

i64_t lambda_fmt_into(obj_p *dst, i64_t limit, obj_p obj)
{
    i64_t n;

    if (as_lambda(obj)->name)
        n = str_fmt_into(dst, limit, "@%s", strof_sym((as_lambda(obj)->name)->i64));
    else
    {
        n = str_fmt_into(dst, 4, "(fn ");
        n += obj_fmt_into(dst, 0, limit, B8_FALSE, as_lambda(obj)->args);
        n += str_fmt_into(dst, 2, " ");
        n += obj_fmt_into(dst, 0, limit, B8_FALSE, as_lambda(obj)->body);
        n += str_fmt_into(dst, 2, ")");
    }

    return n;
}

i64_t obj_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj)
{
    switch (obj->type)
    {
    case -TYPE_B8:
        return b8_fmt_into(dst, obj->b8);
    case -TYPE_U8:
        return byte_fmt_into(dst, obj->u8);
    case -TYPE_I64:
        return i64_fmt_into(dst, obj->i64);
    case -TYPE_F64:
        return f64_fmt_into(dst, obj->f64);
    case -TYPE_SYMBOL:
        return symbol_fmt_into(dst, limit, obj->i64);
    case -TYPE_TIMESTAMP:
        return ts_fmt_into(dst, obj->i64);
    case -TYPE_GUID:
        return guid_fmt_into(dst, as_guid(obj));
    case -TYPE_C8:
        return str_fmt_into(dst, limit, "'%c'", obj->c8 ? obj->c8 : 1);
    case TYPE_B8:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_U8:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_I64:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_F64:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_SYMBOL:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_TIMESTAMP:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_GUID:
        return vector_fmt_into(dst, limit, obj);
    case TYPE_C8:
        return string_fmt_into(dst, limit, obj);
    case TYPE_LIST:
        return list_fmt_into(dst, indent, limit, full, obj);
    case TYPE_ENUM:
        return enum_fmt_into(dst, indent, limit, obj);
    case TYPE_ANYMAP:
        return anymap_fmt_into(dst, indent, limit, full, obj);
    case TYPE_DICT:
        return dict_fmt_into(dst, indent, limit, full, obj);
    case TYPE_TABLE:
        return table_fmt_into(dst, indent, full, obj);
    case TYPE_UNARY:
    case TYPE_BINARY:
    case TYPE_VARY:
        return internal_fmt_into(dst, limit, obj);
    case TYPE_LAMBDA:
        return lambda_fmt_into(dst, limit, obj);
    case TYPE_NULL:
        return str_fmt_into(dst, limit, "null");
    case TYPE_ERROR:
        return error_fmt_into(dst, limit, obj);
    default:
        return str_fmt_into(dst, limit, "@<%s>", type_name(obj->type));
    }
}

obj_p obj_fmt(obj_p obj)
{
    obj_p dst = NULL_OBJ;
    i64_t limit = MAX_ROW_WIDTH;

    obj_fmt_into(&dst, 0, limit, B8_TRUE, obj);

    return dst;
}

/*
 * Format a list of objs into a string
 * using format string as a template with
 * '%' placeholders.
 */
obj_p obj_fmt_n(obj_p *x, u64_t n)
{
    u64_t i;
    i64_t sz = 0;
    str_p p, start = NULL, end = NULL;
    obj_p res = NULL, *b = x;

    if (n == 0)
        return NULL;

    if (n == 1)
        return obj_fmt(*b);

    if ((*b)->type != TYPE_C8)
        return NULL;

    p = as_string(*b);
    sz = strlen(p);
    start = p;
    n -= 1;

    for (i = 0; i < n; i++)
    {
        b += 1;
        end = (str_p)memchr(start, '%', sz);

        if (!end)
        {
            if (res)
                heap_free(res);

            return NULL;
        }

        if (end > start)
            str_fmt_into(&res, (end - start) + 1, "%s", start);

        sz -= (end + 1 - start);
        start = end + 1;

        obj_fmt_into(&res, 0, NO_LIMIT, B8_TRUE, *b);
    }

    if (sz > 0 && memchr(start, '%', sz))
    {
        if (res)
            heap_free(res);

        return NULL;
    }

    str_fmt_into(&res, end - start, "%s", start);

    return res;
}
