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
#include "date.h"
#include "time.h"
#include "timestamp.h"
#include "unary.h"
#include "binary.h"
#include "items.h"
#include "io.h"
#include "error.h"
#include "filter.h"
#include "chrono.h"

#define FORMAT_TRAILER_SIZE 4
#define ERR_STACK_MAX_HEIGHT 10  // Maximum number of error stack frames
#define MAX_ERROR_LEN 4096
#define NO_LIMIT -1

#define TABLE_MAX_WIDTH 10        // Maximum number of columns
#define TABLE_MAX_HEIGHT 20       // Maximum number of rows
#define LIST_MAX_HEIGHT 50        // Maximum number of list/dict rows
#define DEFAULT_MAX_ROW_WIDTH 80  // Maximum number of characters in a row
#define DEFAULT_F64_PRECISION 2   // Number of decimal places for floating point numbers

static u32_t MAX_ROW_WIDTH = DEFAULT_MAX_ROW_WIDTH;
static u32_t F64_PRECISION = DEFAULT_F64_PRECISION;

const lit_p unicode_glyphs[] = {"│", "─", "┌", "┐", "└", "┘", "├", "┤", "┬", "┴", "┼",
                                "↪", "∶", "‾", "•", "╭", "╰", "╮", "╯", "┆", "…", "█"};
const lit_p ascii_glyphs[] = {"|", "-", "+", "+", "+", "+", "+", "+", "+", "+", "+",
                              ">", ":", "~", "*", "|", "|", "|", "|", ":", ".", "|"};

static b8_t __USE_UNICODE = B8_TRUE;

obj_p ray_set_fpr(obj_p x) {
    if (x->type != -TYPE_I64)
        return error(ERR_TYPE, "ray_set_fpr: expected 'i64, got %s", type_name(x->type));

    if (x->i64 < 0)
        F64_PRECISION = DEFAULT_F64_PRECISION;
    else
        F64_PRECISION = x->i64;

    return NULL_OBJ;
}

typedef enum {
    GLYPH_VLINE,
    GLYPH_HLINE,
    GLYPH_TL_CORNER,
    GLYPH_TR_CORNER,
    GLYPH_BL_CORNER,
    GLYPH_BR_CORNER,
    GLYPH_L_TEE,
    GLYPH_R_TEE,
    GLYPH_T_TEE,
    GLYPH_B_TEE,
    GLYPH_CROSS,
    GLYPH_R_ARROW,
    GLYPH_COLON,
    GLYPH_WAVE,
    GLYPH_ASTERISK,
    GLYPH_TL_CURLY,
    GLYPH_BL_CURLY,
    GLYPH_TR_CURLY,
    GLYPH_BR_CURLY,
    GLYPH_VDOTS,
    GLYPH_HDOTS,
    GLYPH_V_BLOCK
} glyph_t;

#define MAXN(n, e)         \
    {                      \
        i64_t k = e;       \
        n = n > k ? n : k; \
    }

i64_t prompt_fmt_into(obj_p *dst) {
    return (__USE_UNICODE) ? str_fmt_into(dst, NO_LIMIT, "%s%s %s", GREEN, unicode_glyphs[GLYPH_R_ARROW], RESET)
                           : str_fmt_into(dst, NO_LIMIT, "%s%s %s", GREEN, ascii_glyphs[GLYPH_R_ARROW], RESET);
}

nil_t debug_str(obj_p str) {
    u64_t i, l;
    str_p s;

    l = str->len;
    s = AS_C8(str);

    printf("** s: ");
    for (i = 0; i < l; i++)
        printf("0x%hhx ", s[i]);

    printf("\n");
}

b8_t limit_reached(i64_t limit, i64_t n) { return limit != NO_LIMIT && n >= limit; }

/*
 * return n:
 * n > limit - truncated
 * n < limit - fits into buffer
 * n < 0 - error
 */
i64_t str_vfmt_into(obj_p *dst, i64_t limit, lit_p fmt, va_list vargs) {
    str_p s;
    i64_t n = 0, l, o, size;
    va_list args;

    if (limit == 0)
        return 0;

    size = 2;  // start from 2 symbols: 1 for the string and 1 for the null terminator

    // Allocate or expand the buffer if necessary
    if (*dst == NULL_OBJ) {
        *dst = C8(size);
        if (*dst == NULL_OBJ)
            PANIC("str_vfmt_into: OOM");

        l = 0;
    } else {
        l = (*dst)->len;
        if (l > 0 && AS_C8(*dst)[l - 1] == '\0')
            l--;
        if (is_null(resize_obj(dst, l + size))) {
            heap_free(*dst);
            PANIC("str_vfmt_into: OOM");
        }
    }

    o = l;

    while (B8_TRUE) {
        s = AS_C8(*dst) + o;
        va_copy(args, vargs);  // Make a copy of args to use with vsnprintf
        n = vsnprintf(s, size, fmt, args);
        va_end(args);  // args should be ended, not vargs

        if (n < 0) {
            heap_free(*dst);
            PANIC("str_vfmt_into: Error in vsnprintf");
        }

        if (n < size) {
            resize_obj(dst, l + n);  // resize the buffer to fit the string
            return n;                // n fits into buffer, return n
        }

        // n >= size, the buffer is too small, but we reached the limit
        if (limit != NO_LIMIT && n >= limit)
            return limit;

        // Expand the buffer for the next iteration
        size = n + 1;  // +1 for the null terminator
        if (is_null(resize_obj(dst, l + size))) {
            heap_free(*dst);
            PANIC("str_vfmt_into: OOM");
        }
    }
}

i64_t str_fmt_into(obj_p *dst, i64_t limit, lit_p fmt, ...) {
    i64_t n;
    va_list args;

    va_start(args, fmt);
    n = str_vfmt_into(dst, limit, fmt, args);
    va_end(args);

    return n;
}

i64_t str_fmt_into_n(obj_p *dst, i64_t limit, i64_t repeat, lit_p fmt, ...) {
    i64_t i, n;

    for (i = 0, n = 0; i < repeat; i++)
        n += str_fmt_into(dst, limit, fmt);

    return n;
}

obj_p str_vfmt(i64_t limit, lit_p fmt, va_list vargs) {
    i64_t n = 0, size;
    obj_p res;
    str_p s;
    va_list args;

    if (limit == 0)
        return NULL_OBJ;

    size = 2;

    res = C8(size);

    if (is_null(res))
        PANIC("str_vfmt: OOM");

    while (B8_TRUE) {
        s = AS_C8(res);
        va_copy(args, vargs);  // Make a copy of args to use with vsnprintf
        n = vsnprintf(s, size, fmt, args);
        va_end(vargs);

        if (n < 0) {
            // Handle encoding error
            heap_free(res);
            return NULL_OBJ;
        }

        if (n < size)
            break;  // Buffer is large enough

        if (limit > 0)
            return res;  // We have a limit and it's reached

        size = n + 1;  // Increase buffer size
        res = resize_obj(&res, size);

        if (is_null(res))
            PANIC("str_vfmt: OOM");
    }

    return res;
}

obj_p str_fmt(i64_t limit, lit_p fmt, ...) {
    obj_p res;
    va_list args;

    va_start(args, fmt);
    res = str_vfmt(limit, fmt, args);
    va_end(args);

    return res;
}

i64_t glyph_fmt_into(obj_p *dst, glyph_t glyph, b8_t unicode) {
    if (unicode)
        return str_fmt_into(dst, 4, "%s", unicode_glyphs[glyph]);

    switch (glyph) {
        case GLYPH_HDOTS:
            return str_fmt_into(dst, 4, "...");
        default:
            return str_fmt_into(dst, 2, "%s", ascii_glyphs[glyph]);
    }
}

i64_t b8_fmt_into(obj_p *dst, b8_t val) {
    if (val)
        return str_fmt_into(dst, 5, "true");

    return str_fmt_into(dst, 6, "false");
}

i64_t byte_fmt_into(obj_p *dst, u8_t val) { return str_fmt_into(dst, 5, "0x%02x", val); }

i64_t c8_fmt_into(obj_p *dst, b8_t full, c8_t val) {
    switch (val) {
        case '\"':
            return full ? str_fmt_into(dst, 5, "'\\\"'") : str_fmt_into(dst, 4, "\\\"");
        case '\n':
            return full ? str_fmt_into(dst, 5, "'\\n'") : str_fmt_into(dst, 3, "\\n");
        case '\r':
            return full ? str_fmt_into(dst, 5, "'\\r'") : str_fmt_into(dst, 3, "\\r");
        case '\t':
            return full ? str_fmt_into(dst, 5, "'\\t'") : str_fmt_into(dst, 3, "\\t");
        case '\0':
            return full ? str_fmt_into(dst, 3, "''") : str_fmt_into(dst, 2, " ");
        default:
            // Handle control characters (ASCII 1-31) with \xxx notation
            if (val > 0 && val < 32)
                return full ? str_fmt_into(dst, 8, "'\\%03o'", (u8_t)val) : str_fmt_into(dst, 6, "\\%03o", (u8_t)val);

            return full ? str_fmt_into(dst, 4, "'%c'", val) : str_fmt_into(dst, 2, "%c", val);
    }
}

i64_t i16_fmt_into(obj_p *dst, i16_t val) {
    if (val == NULL_I16)
        return str_fmt_into(dst, 4, "%s", LIT_NULL_I16);

    return str_fmt_into(dst, NO_LIMIT, "%d", val);
}

i64_t i32_fmt_into(obj_p *dst, i32_t val) {
    if (val == NULL_I32)
        return str_fmt_into(dst, 4, "%s", LIT_NULL_I32);

    return str_fmt_into(dst, NO_LIMIT, "%d", val);
}

i64_t i64_fmt_into(obj_p *dst, i64_t val) {
    if (val == NULL_I64)
        return str_fmt_into(dst, 4, "%s", LIT_NULL_I64);

    return str_fmt_into(dst, NO_LIMIT, "%lld", val);
}

i64_t f64_fmt_into(obj_p *dst, f64_t val) {
    f64_t order;

    if (ops_is_nan(val))
        return str_fmt_into(dst, 4, LIT_NULL_F64);
    if (val == -0.0)
        val = 0.0;

    // Find the order of magnitude of the number to select the appropriate format
    order = log10(val < 0 ? -val : val);

    if (val && (order > 6 || order < -1))
        return str_fmt_into(dst, NO_LIMIT, "%.*e", 3 * F64_PRECISION, val);

    return str_fmt_into(dst, NO_LIMIT, "%.*f", F64_PRECISION, val);
}

i64_t date_fmt_into(obj_p *dst, i32_t val) {
    datestruct_t dt;

    if (val == NULL_I32)
        return str_fmt_into(dst, 4, LIT_NULL_DATE);

    dt = date_from_i32(val);

    return str_fmt_into(dst, NO_LIMIT, "%.4d.%.2d.%.2d", dt.year, dt.month, dt.day);
}

i64_t time_fmt_into(obj_p *dst, i32_t val) {
    timestruct_t tm;

    if (val == NULL_I32)
        return str_fmt_into(dst, 4, LIT_NULL_TIME);

    tm = time_from_i32(val);
    if (tm.sign == -1)
        return str_fmt_into(dst, NO_LIMIT, "-%.2d:%.2d:%.2d.%.3d", tm.hours, tm.mins, tm.secs, tm.msecs);

    return str_fmt_into(dst, NO_LIMIT, "%.2d:%.2d:%.2d.%.3d", tm.hours, tm.mins, tm.secs, tm.msecs);
}

i64_t timestamp_fmt_into(obj_p *dst, i64_t val) {
    timestamp_t ts;

    if (val == NULL_I64)
        return str_fmt_into(dst, 4, LIT_NULL_TIMESTAMP);

    ts = timestamp_from_i64(val);

    return str_fmt_into(dst, NO_LIMIT, "%.4d.%.2d.%.2dD%.2d:%.2d:%.2d.%.9d", ts.year, ts.month, ts.day, ts.hours,
                        ts.mins, ts.secs, ts.nanos);
}

i64_t guid_fmt_into(obj_p *dst, guid_t *val) {
    i64_t n;

    if (memcmp(*val, NULL_GUID, sizeof(guid_t)) == 0)
        return str_fmt_into(dst, 4, LIT_NULL_GUID);

    n = str_fmt_into(dst, 37, "%02hhx%02hhx%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", (*val)[0],
                     (*val)[1], (*val)[2], (*val)[3], (*val)[4], (*val)[5], (*val)[6], (*val)[7], (*val)[8], (*val)[9],
                     (*val)[10], (*val)[11], (*val)[12], (*val)[13], (*val)[14], (*val)[15]);

    return n;
}

i64_t symbol_fmt_into(obj_p *dst, i64_t limit, b8_t full, i64_t val) {
    if (val == NULL_I64)
        return full ? str_fmt_into(dst, 4, LIT_NULL_SYMBOL) : str_fmt_into(dst, 1, "");

    i64_t n = str_fmt_into(dst, limit, "%s", str_from_symbol(val));
    if (limit_reached(limit, n))
        n += str_fmt_into(dst, 3, "..");

    return n;
}

i64_t string_fmt_into(obj_p *dst, i64_t limit, b8_t full, obj_p obj) {
    i64_t n;
    u64_t i, l;
    str_p s;

    n = 0;

    if (full)
        n += str_fmt_into(dst, 2, "\"");

    l = obj->len;
    s = AS_C8(obj);

    for (i = 0; i < l; i++) {
        n += c8_fmt_into(dst, B8_FALSE, s[i]);
        if (limit_reached(limit, n))
            break;
    }

    if (limit_reached(limit, n))
        n += str_fmt_into(dst, 3, "..");

    if (full)
        n += str_fmt_into(dst, 2, "\"");

    return n;
}

i64_t error_frame_fmt_into(obj_p *dst, obj_p obj, i64_t idx, str_p msg, i32_t msg_len) {
    i64_t n, done = 0;
    u32_t line_len, fname_len;
    i32_t frame_len, linecode_len;
    u16_t line_number = 0, i;
    lit_p filename, source, function, start, end, lf = "", flname = "repl", fnname = "anonymous";
    obj_p *frame = AS_LIST(obj);
    span_t span = (span_t){0};
    b8_t unicode = __USE_UNICODE;

    span.id = frame[0]->i64;
    if (frame[1] != NULL_OBJ) {
        filename = AS_C8(frame[1]);
        fname_len = frame[1]->len;
    } else {
        filename = flname;
        fname_len = strlen(flname);
    }
    function = (frame[2] != NULL_OBJ) ? str_from_symbol(frame[2]->i64) : fnname;
    source = AS_C8(frame[3]);
    line_len = frame[3]->len;

    n = str_fmt_into(dst, MAX_ERROR_LEN, "%s", CYAN);
    n += glyph_fmt_into(dst, GLYPH_TL_CURLY, unicode);
    n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);
    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s", RESET);
    frame_len = n;
    n += str_fmt_into(dst, MAX_ERROR_LEN, "[%lld]", idx);
    frame_len = n - frame_len;
    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s", CYAN);
    n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);
    n += glyph_fmt_into(dst, GLYPH_T_TEE, unicode);
    n += str_fmt_into(dst, MAX_ERROR_LEN, "%s", RESET);
    n += str_fmt_into(dst, MAX_ERROR_LEN, " %.*s:%d..%d in function: @%s\n", fname_len, filename, span.start_line + 1,
                      span.end_line + 1, function);

    start = source;
    end = NULL;

    for (;;) {
        end = (str_p)memchr(start, '\n', line_len);

        if (end == NULL) {
            done = 1;
            end = source + strlen(source) - 1;
            lf = "\n";
        }

        line_len = end - start + 1;

        if (line_number >= span.start_line && line_number <= span.end_line) {
            n += str_fmt_into(dst, NO_LIMIT, "%s", CYAN);
            n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
            n += str_fmt_into(dst, NO_LIMIT, "%s", RESET);
            linecode_len = n;
            n += str_fmt_into(dst, NO_LIMIT, " %d ", line_number + 1);
            linecode_len = n - linecode_len;
            n += str_fmt_into(dst, NO_LIMIT, "%s", CYAN);

            for (i = 0; i < frame_len - 1; i++)
                n += str_fmt_into(dst, 2, " ");

            n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
            n += str_fmt_into(dst, NO_LIMIT, "%s %.*s", RESET, line_len, start);

            if (span.start_line == span.end_line) {
                n += str_fmt_into(dst, NO_LIMIT, "%s%s", lf, CYAN);
                n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);

                for (i = 0; i < linecode_len + frame_len - 1; i++)
                    n += str_fmt_into(dst, 2, " ");

                n += glyph_fmt_into(dst, GLYPH_B_TEE, unicode);
                n += str_fmt_into(dst, NO_LIMIT, "%s ", RESET);
                for (i = 0; i < span.start_column; i++)
                    n += str_fmt_into(dst, 2, " ");

                for (i = span.start_column; i < span.end_column; i++) {
                    n += str_fmt_into(dst, NO_LIMIT, "%s", TOMATO);
                    n += glyph_fmt_into(dst, GLYPH_WAVE, unicode);
                    n += str_fmt_into(dst, NO_LIMIT, "%s", RESET);
                }

                n += str_fmt_into(dst, NO_LIMIT, "%s", TOMATO);
                n += glyph_fmt_into(dst, GLYPH_WAVE, unicode);
                n += str_fmt_into(dst, NO_LIMIT, " %.*s%s\n", msg_len, msg, RESET);
            } else {
                if (line_number == span.start_line) {
                    n += str_fmt_into(dst, NO_LIMIT, "      %s", CYAN);
                    n += glyph_fmt_into(dst, GLYPH_COLON, unicode);
                    n += str_fmt_into(dst, NO_LIMIT, "%s ", RESET);
                    for (i = 0; i < span.start_column; i++)
                        n += str_fmt_into(dst, 2, " ");

                    n += str_fmt_into(dst, NO_LIMIT, "%s", TOMATO);
                    n += glyph_fmt_into(dst, GLYPH_WAVE, unicode);
                    n += str_fmt_into(dst, NO_LIMIT, "%s\n", RESET);
                } else if (line_number == span.end_line) {
                    n += str_fmt_into(dst, NO_LIMIT, "%s%s      ", lf, CYAN);
                    n += glyph_fmt_into(dst, GLYPH_COLON, unicode);
                    n += str_fmt_into(dst, NO_LIMIT, "%s ", RESET);
                    for (i = 0; i < span.end_column + 1; i++)
                        n += str_fmt_into(dst, 2, " ");

                    n += str_fmt_into(dst, NO_LIMIT, "%s", TOMATO);
                    n += glyph_fmt_into(dst, GLYPH_WAVE, unicode);
                    n += str_fmt_into(dst, NO_LIMIT, " %.*s%s\n", msg_len, msg, RESET);
                }
            }
        }

        if (line_number > span.end_line || done)
            break;

        line_number++;
        start = end + 1;
    }

    return n;
}

i64_t error_fmt_into(obj_p *dst, i64_t limit, obj_p obj) {
    i64_t n;
    i32_t msg_len;
    u16_t i, l, m;
    lit_p error_desc;
    str_p msg;
    ray_error_p error = AS_ERROR(obj);

    switch (error->code) {
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
        case ERR_OS:
            error_desc = "os";
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
            error_desc = "corrupted object/unknown error";
    }

    msg_len = (i32_t)error->msg->len;
    msg = AS_C8(error->msg);

    // there is a locations
    if (error->locs != NULL_OBJ) {
        n = str_fmt_into(dst, MAX_ERROR_LEN, "%s", TOMATO);
        n += glyph_fmt_into(dst, GLYPH_ASTERISK, __USE_UNICODE);
        n += glyph_fmt_into(dst, GLYPH_ASTERISK, __USE_UNICODE);
        n += str_fmt_into(dst, MAX_ERROR_LEN, " [E%.3lld] error%s: %s\n", error->code, RESET, error_desc);

        l = error->locs->len;
        m = l > ERR_STACK_MAX_HEIGHT ? ERR_STACK_MAX_HEIGHT : l;

        for (i = 0; i < m; i++) {
            n += error_frame_fmt_into(dst, AS_LIST(error->locs)[i], l - i - 1, msg, msg_len);
            msg_len = 0;
        }

        if (l > m) {
            n += str_fmt_into(dst, limit, "  ..\n");
            n += error_frame_fmt_into(dst, AS_LIST(error->locs)[l - 1], 0, msg, msg_len);
        }
        return n;
    }

    n = str_fmt_into(dst, MAX_ERROR_LEN, "%s", TOMATO);
    n += glyph_fmt_into(dst, GLYPH_ASTERISK, __USE_UNICODE);
    n += glyph_fmt_into(dst, GLYPH_ASTERISK, __USE_UNICODE);
    n +=
        str_fmt_into(dst, MAX_ERROR_LEN, " [E%.3lld] error%s: %s %.*s\n", error->code, RESET, error_desc, msg_len, msg);

    return n;
}

i64_t raw_fmt_into(obj_p *dst, i64_t indent, i64_t limit, obj_p obj, i64_t i) {
    obj_p idx, res;
    i64_t n;

    switch (obj->type) {
        case TYPE_B8:
            return b8_fmt_into(dst, AS_B8(obj)[i]);
        case TYPE_U8:
            return byte_fmt_into(dst, AS_U8(obj)[i]);
        case TYPE_I32:
            return i32_fmt_into(dst, AS_I32(obj)[i]);
        case TYPE_DATE:
            return date_fmt_into(dst, AS_DATE(obj)[i]);
        case TYPE_TIME:
            return time_fmt_into(dst, AS_TIME(obj)[i]);
        case TYPE_I64:
            return i64_fmt_into(dst, AS_I64(obj)[i]);
        case TYPE_F64:
            return f64_fmt_into(dst, AS_F64(obj)[i]);
        case TYPE_SYMBOL:
            return symbol_fmt_into(dst, limit, B8_TRUE, AS_SYMBOL(obj)[i]);
        case TYPE_TIMESTAMP:
            return timestamp_fmt_into(dst, AS_TIMESTAMP(obj)[i]);
        case TYPE_GUID:
            return guid_fmt_into(dst, &AS_GUID(obj)[i]);
        case TYPE_C8:
            return c8_fmt_into(dst, B8_TRUE, AS_C8(obj)[i]);
        case TYPE_LIST:
            return obj_fmt_into(dst, indent, limit, B8_FALSE, AS_LIST(obj)[i]);
        case TYPE_MAPFILTER:
            res = at_idx(AS_LIST(obj)[0], AS_I64(AS_LIST(obj)[1])[i]);
            n = obj_fmt_into(dst, indent, limit, B8_FALSE, res);
            drop_obj(res);
            return n;
        case TYPE_ENUM:
        case TYPE_MAPLIST:
            idx = i64(i);
            res = ray_at(obj, idx);
            drop_obj(idx);
            if (IS_ERROR(res)) {
                drop_obj(res);
                return error_fmt_into(dst, limit, res);
            }
            n = obj_fmt_into(dst, indent, limit, B8_FALSE, res);
            drop_obj(res);
            return n;
        case TYPE_PARTEDLIST:
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDGUID:
        case TYPE_PARTEDENUM:
        case TYPE_MAPCOMMON:
            res = at_idx(obj, i);
            n = obj_fmt_into(dst, indent, limit, B8_FALSE, res);
            drop_obj(res);
            return n;
        default:
            return str_fmt_into(dst, limit, "null");
    }
}

i64_t vector_fmt_into(obj_p *dst, i64_t limit, obj_p obj) {
    if (obj->len == 0)
        return str_fmt_into(dst, limit, "[]");

    i64_t i, n = str_fmt_into(dst, limit, "["), indent = 0;
    i64_t l;

    if (limit_reached(limit, n))
        return n;

    l = obj->len;

    for (i = 0; i < l; i++) {
        n += raw_fmt_into(dst, indent, limit, obj, i);
        if (limit_reached(limit, n))
            break;

        if (i + 1 < l)
            n += str_fmt_into(dst, 2, " ");

        if (limit_reached(limit, n))
            break;
    }

    if (limit_reached(limit, n))
        n += str_fmt_into(dst, 4, "..]");
    else
        n += str_fmt_into(dst, 2, "]");

    return n;
}

i64_t list_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj) {
    i64_t i, n, list_height = obj->len;

    if (list_height == 0)
        return str_fmt_into(dst, 3, "()");

    n = str_fmt_into(dst, 2, "(");

    if (list_height > LIST_MAX_HEIGHT)
        list_height = LIST_MAX_HEIGHT;

    if (!full) {
        for (i = 0; i < list_height - 1; i++) {
            n += obj_fmt_into(dst, indent, limit, B8_FALSE, AS_LIST(obj)[i]);
            n += str_fmt_into(dst, 2, " ");
        }

        n += obj_fmt_into(dst, indent, limit, B8_FALSE, AS_LIST(obj)[i]);

        if (list_height < (i64_t)obj->len)
            n += str_fmt_into(dst, 3, "..");

        n += str_fmt_into(dst, 2, ")");

        return n;
    }

    indent += 2;

    for (i = 0; i < list_height; i++) {
        n += str_fmt_into(dst, 2, "\n");
        n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
        n += obj_fmt_into(dst, indent, limit, B8_FALSE, AS_LIST(obj)[i]);
    }

    if (list_height < (i64_t)obj->len) {
        n += str_fmt_into(dst, 2, "\n");
        n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
        n += str_fmt_into(dst, 3, "..");
    }

    indent -= 2;

    n += str_fmt_into(dst, 2, "\n");
    n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
    n += str_fmt_into(dst, 2, ")");

    return n;
}

i64_t enum_fmt_into(obj_p *dst, i64_t indent, i64_t limit, obj_p obj) {
    i64_t n;
    obj_p s, e, idx;

    s = ray_key(obj);

    if (IS_ERROR(s))
        return error_fmt_into(dst, limit, s);

    if (ENUM_VAL(obj)->len >= TABLE_MAX_HEIGHT) {
        limit = TABLE_MAX_HEIGHT;
        idx = i64(TABLE_MAX_HEIGHT);
        e = ray_take(idx, obj);
        drop_obj(idx);
    } else
        e = ray_value(obj);

    if (IS_ERROR(e)) {
        drop_obj(s);
        return error_fmt_into(dst, limit, e);
    }

    n = str_fmt_into(dst, 2, "'");
    n += obj_fmt_into(dst, indent, limit, B8_FALSE, s);
    n += str_fmt_into(dst, 2, "#");
    n += obj_fmt_into(dst, indent, limit, B8_FALSE, e);

    drop_obj(s);
    drop_obj(e);

    return n;
}

i64_t anymap_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj) {
    i64_t n;
    obj_p a, idx;

    if (MAPLIST_VAL(obj)->len >= TABLE_MAX_HEIGHT) {
        limit = TABLE_MAX_HEIGHT;
        idx = i64(TABLE_MAX_HEIGHT);
        a = ray_take(idx, obj);
        drop_obj(idx);
    } else
        a = ray_value(obj);

    n = obj_fmt_into(dst, indent, limit, full, a);

    drop_obj(a);

    return n;
}

i64_t dict_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj) {
    obj_p keys = AS_LIST(obj)[0], vals = AS_LIST(obj)[1];
    i64_t n;
    u64_t i, dict_height = ops_count(keys);

    if (dict_height == 0)
        return str_fmt_into(dst, 3, "{}");

    n = str_fmt_into(dst, 2, "{");

    if (dict_height > LIST_MAX_HEIGHT)
        dict_height = LIST_MAX_HEIGHT;

    if (!full) {
        for (i = 0; i < dict_height - 1; i++) {
            MAXN(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, keys, i));
            MAXN(n, str_fmt_into(dst, MAX_ROW_WIDTH, ": "));
            MAXN(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, vals, i));
            MAXN(n, str_fmt_into(dst, MAX_ROW_WIDTH, " "));
        }

        MAXN(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, keys, i));
        MAXN(n, str_fmt_into(dst, MAX_ROW_WIDTH, ": "));
        MAXN(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, vals, i));

        if (dict_height < ops_count(keys))
            MAXN(n, str_fmt_into(dst, 3, ".."));

        MAXN(n, str_fmt_into(dst, 2, "}"));

        return n;
    }

    indent += 2;

    for (i = 0; i < dict_height; i++) {
        MAXN(n, str_fmt_into(dst, 2, "\n"));
        MAXN(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
        MAXN(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, keys, i));
        n += str_fmt_into(dst, MAX_ROW_WIDTH, ": ");
        MAXN(n, raw_fmt_into(dst, indent, MAX_ROW_WIDTH, vals, i));
    }

    if (dict_height < ops_count(keys)) {
        MAXN(n, str_fmt_into(dst, 2, "\n"));
        MAXN(n, str_fmt_into_n(dst, NO_LIMIT, indent, " "));
        MAXN(n, str_fmt_into(dst, 3, ".."));
    }

    indent -= 2;

    MAXN(n, str_fmt_into(dst, 2, "\n"));
    MAXN(n, str_fmt_into_n(dst, limit, indent, " "));
    MAXN(n, str_fmt_into(dst, 2, "}"));

    return n;
}

i64_t table_fmt_into(obj_p *dst, i64_t indent, b8_t full, obj_p obj) {
    i64_t *header, i, j, n, m, l, table_width, table_height, total_width, rows, cols;
    obj_p s, column, columns = AS_LIST(obj)[1], column_widths, footer,
                     formatted_columns[TABLE_MAX_WIDTH][TABLE_MAX_HEIGHT] = {{NULL_OBJ}};
    str_p p;
    b8_t unicode = __USE_UNICODE;

    header = AS_SYMBOL(AS_LIST(obj)[0]);
    total_width = 0;

    if (!full) {
        n = str_fmt_into(dst, 8, "(table ");
        n += obj_fmt_into(dst, indent, MAX_ROW_WIDTH, B8_FALSE, AS_LIST(obj)[0]);
        n += glyph_fmt_into(dst, GLYPH_HDOTS, unicode);
        n += str_fmt_into(dst, 2, ")");

        return n;
    }

    cols = (AS_LIST(obj)[0])->len;
    table_width = cols;
    if (table_width == 0)
        return str_fmt_into(dst, 7, "@table");

    if (table_width > TABLE_MAX_WIDTH)
        table_width = TABLE_MAX_WIDTH;

    rows = ops_count(obj);
    table_height = rows;
    if (table_height > TABLE_MAX_HEIGHT)
        table_height = TABLE_MAX_HEIGHT;

    column_widths = I64(table_width);

    // Calculate each column maximum width
    for (i = 0; i < table_width; i++) {
        // First check the column name
        l = strlen(str_from_symbol(header[i]));

        // Then traverse first n elements of column
        for (j = 0; j < table_height / 2; j++) {
            column = AS_LIST(columns)[i];
            s = NULL_OBJ;
            m = raw_fmt_into(&s, 0, 38, column, j);
            formatted_columns[i][j] = s;
            MAXN(l, m);
        }

        // Traverse the rest of the column
        for (; j < table_height; j++) {
            column = AS_LIST(columns)[i];
            s = NULL_OBJ;
            if (table_height == rows)
                m = raw_fmt_into(&s, 0, 38, column, j);
            else {
                m = raw_fmt_into(&s, 0, 38, column, rows - table_height + j);
            }
            formatted_columns[i][j] = s;
            MAXN(l, m);
        }

        AS_I64(column_widths)[i] = l + 2;  // Add 2 for padding
        total_width += AS_I64(column_widths)[i];
    }

    total_width += table_width - 1;

    // calculate width of the footer
    footer =
        str_fmt(NO_LIMIT, " %lld rows (%lld shown) %lld columns (%lld shown)", rows, table_height, cols, table_width);

    if (total_width < (i64_t)footer->len) {
        AS_I64(column_widths)[table_width - 1] += (i64_t)footer->len - total_width;
        total_width = (i64_t)footer->len;

        for (i = 0; i < table_width - 1; i++) {
            // Adjust width to '...' if it's less than 4
            if (AS_I64(column_widths)[i] < 4) {
                total_width += 4 - AS_I64(column_widths)[i];
                AS_I64(column_widths)[i] = 4;
            }
        }
    }

    // Top border of the table
    n = glyph_fmt_into(dst, GLYPH_TL_CORNER, unicode);
    for (i = 0; i < table_width; i++) {
        for (j = 0; j < AS_I64(column_widths)[i]; j++)
            n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);
        if (i < table_width - 1)
            n += glyph_fmt_into(dst, GLYPH_T_TEE, unicode);
        else
            n += glyph_fmt_into(dst, GLYPH_TR_CORNER, unicode);
    }

    // Print table header
    n += str_fmt_into(dst, 2, "\n");
    n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
    n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
    for (i = 0; i < table_width; i++) {
        p = str_from_symbol(header[i]);
        m = AS_I64(column_widths)[i] - strlen(p) - 2;
        n += str_fmt_into(dst, NO_LIMIT, " %s ", p);
        n += str_fmt_into_n(dst, NO_LIMIT, m, " ");
        n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
    }

    // Separator between header and rows
    n += str_fmt_into(dst, 2, "\n");
    n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
    n += glyph_fmt_into(dst, GLYPH_L_TEE, unicode);
    for (i = 0; i < table_width; i++) {
        for (j = 0; j < AS_I64(column_widths)[i]; j++)
            n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);
        if (i < table_width - 1)
            n += glyph_fmt_into(dst, GLYPH_CROSS, unicode);
        else
            n += glyph_fmt_into(dst, GLYPH_R_TEE, unicode);
    }

    // Print table content
    for (j = 0; j < table_height; j++) {
        n += str_fmt_into(dst, 2, "\n");
        n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");

        // Indicate missing rows
        if (j == table_height / 2 && table_height != rows) {
            n += glyph_fmt_into(dst, GLYPH_VDOTS, unicode);
            for (i = 0; i < table_width; i++) {
                n += str_fmt_into(dst, 2, " ");
                l = n;
                n += glyph_fmt_into(dst, GLYPH_HDOTS, unicode);
                l = n - l;
                if (unicode)
                    m = AS_I64(column_widths)[i] - l + 1;
                else
                    m = AS_I64(column_widths)[i] - l - 1;
                n += str_fmt_into_n(dst, NO_LIMIT, m, " ");
                n += glyph_fmt_into(dst, GLYPH_VDOTS, unicode);
            }
            n += str_fmt_into(dst, 2, "\n");
        }

        n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);

        for (i = 0; i < table_width; i++) {
            s = formatted_columns[i][j];
            p = AS_C8(s);
            m = AS_I64(column_widths)[i] - s->len - 2;
            n += str_fmt_into(dst, s->len + 3, " %.*s ", (i32_t)s->len, p);
            n += str_fmt_into_n(dst, NO_LIMIT, m, " ");
            n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
            // Free formatted column
            drop_obj(s);
        }
    }

    // Bottom border of the table
    n += str_fmt_into(dst, 2, "\n");
    n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
    n += glyph_fmt_into(dst, GLYPH_L_TEE, unicode);
    for (i = 0; i < table_width; i++) {
        for (j = 0; j < AS_I64(column_widths)[i]; j++)
            n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);
        if (i < table_width - 1)
            n += glyph_fmt_into(dst, GLYPH_B_TEE, unicode);
        else
            n += glyph_fmt_into(dst, GLYPH_R_TEE, unicode);
    }

    drop_obj(column_widths);

    // Draw info footer
    n += str_fmt_into(dst, 2, "\n");
    n += str_fmt_into_n(dst, NO_LIMIT, indent, " ");
    n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
    m = n;
    n += str_fmt_into(dst, NO_LIMIT, "%.*s", (i32_t)footer->len, AS_C8(footer));
    drop_obj(footer);
    m = n - m;

    for (i = 0; i < total_width - m; i++)
        n += str_fmt_into(dst, 2, " ");

    n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
    n += str_fmt_into(dst, 2, "\n");

    m = n;
    n += glyph_fmt_into(dst, GLYPH_BL_CORNER, unicode);
    for (i = 0; i < total_width; i++)
        n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);

    n += glyph_fmt_into(dst, GLYPH_BR_CORNER, unicode);

    return n;
}

i64_t internal_fmt_into(obj_p *dst, i64_t limit, obj_p obj) {
    return str_fmt_into(dst, limit, "%s", env_get_internal_name(obj));
}

i64_t lambda_fmt_into(obj_p *dst, i64_t limit, obj_p obj) {
    i64_t n;

    if (AS_LAMBDA(obj)->name)
        n = str_fmt_into(dst, limit, "@%s", str_from_symbol((AS_LAMBDA(obj)->name)->i64));
    else {
        n = str_fmt_into(dst, 4, "(fn ");
        n += obj_fmt_into(dst, 0, limit, B8_FALSE, AS_LAMBDA(obj)->args);
        n += str_fmt_into(dst, 2, " ");
        n += obj_fmt_into(dst, 0, limit, B8_FALSE, AS_LAMBDA(obj)->body);
        n += str_fmt_into(dst, 2, ")");
    }

    return n;
}

i64_t obj_fmt_into(obj_p *dst, i64_t indent, i64_t limit, b8_t full, obj_p obj) {
    switch (obj->type) {
        case -TYPE_B8:
            return b8_fmt_into(dst, obj->b8);
        case -TYPE_C8:
            return c8_fmt_into(dst, full, obj->c8);
        case -TYPE_U8:
            return byte_fmt_into(dst, obj->u8);
        case -TYPE_I16:
            return i16_fmt_into(dst, obj->i16);
        case -TYPE_I32:
            return i32_fmt_into(dst, obj->i32);
        case -TYPE_DATE:
            return date_fmt_into(dst, obj->i32);
        case -TYPE_TIME:
            return time_fmt_into(dst, obj->i32);
        case -TYPE_I64:
            return i64_fmt_into(dst, obj->i64);
        case -TYPE_F64:
            return f64_fmt_into(dst, obj->f64);
        case -TYPE_SYMBOL:
            return symbol_fmt_into(dst, limit, full, obj->i64);
        case -TYPE_TIMESTAMP:
            return timestamp_fmt_into(dst, obj->i64);
        case -TYPE_GUID:
            return guid_fmt_into(dst, AS_GUID(obj));
        case TYPE_B8:
        case TYPE_U8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_F64:
        case TYPE_GUID:
            return vector_fmt_into(dst, limit, obj);
        case TYPE_C8:
            return string_fmt_into(dst, limit, full, obj);
        case TYPE_LIST:
            return list_fmt_into(dst, indent, limit, full, obj);
        case TYPE_ENUM:
            return enum_fmt_into(dst, indent, limit, obj);
        case TYPE_MAPLIST:
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
            return str_fmt_into(dst, limit, "Null");
        case TYPE_ERROR:
            return error_fmt_into(dst, limit, obj);
        default:
            return str_fmt_into(dst, limit, "@<%s>", type_name(obj->type));
    }
}

obj_p obj_fmt(obj_p obj, b8_t full) {
    obj_p dst = NULL_OBJ;
    i64_t limit = MAX_ROW_WIDTH;

    obj_fmt_into(&dst, 0, limit, full, obj);

    return dst;
}

/*
 * Format a list of objs into a string
 * using format string as a template with
 * '%' placeholders.
 */
obj_p obj_fmt_n(obj_p *x, u64_t n) {
    u64_t i;
    i64_t sz = 0;
    str_p p, start = NULL, end = NULL;
    obj_p res = NULL_OBJ;

    if (n == 0)
        return NULL_OBJ;

    if (n == 1)
        return obj_fmt(x[0], x[0]->type != TYPE_C8);

    if (x[0]->type != TYPE_C8)
        return NULL_OBJ;

    p = AS_C8(x[0]);
    sz = x[0]->len;
    start = p;

    for (i = 1; i < n; i++) {
        end = (str_p)memchr(start, '%', sz);

        if (!end) {
            heap_free(res);
            return NULL_OBJ;
        }

        if (end > start)
            str_fmt_into(&res, NO_LIMIT, "%.*s", end - start, start);

        sz -= (end + 1 - start);
        start = end + 1;

        obj_fmt_into(&res, 0, MAX_ROW_WIDTH, B8_FALSE, x[i]);
    }

    if (sz > 0 && memchr(start, '%', sz)) {
        heap_free(res);
        return NULL_OBJ;
    }

    str_fmt_into(&res, NO_LIMIT, "%.*s", sz, start);

    return res;
}

obj_p ray_show(obj_p obj) {
    obj_p dst = NULL_OBJ;

    obj_fmt_into(&dst, 0, NO_LIMIT, B8_TRUE, obj);
    printf("%.*s\n", (i32_t)dst->len, AS_C8(dst));
    drop_obj(dst);

    return NULL_OBJ;
}

i64_t timeit_fmt_into(obj_p *dst, u64_t indent, u64_t *index, timeit_t *timeit) {
    i64_t n;
    u64_t i;
    f64_t elapsed = 0.0;
    b8_t unicode = __USE_UNICODE;
    timeit_span_t span;

    n = 0;

    while (*index < timeit->n) {
        span = timeit->spans[*index];

        switch (span.type) {
            case TIMEIT_SPAN_START:
                for (i = 0; i < indent; i++) {
                    n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
                    n += str_fmt_into(dst, 2, " ");
                }
                n += glyph_fmt_into(dst, GLYPH_TL_CURLY, unicode);
                n += str_fmt_into(dst, NO_LIMIT, " %s\n", span.msg);
                (*index)++;
                n += timeit_fmt_into(dst, indent + 1, index, timeit);
                elapsed = ray_clock_elapsed_ms(&span.clock, &timeit->spans[*index].clock);
                for (i = 0; i < indent; i++) {
                    n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
                    n += str_fmt_into(dst, 2, " ");
                }
                n += glyph_fmt_into(dst, GLYPH_BL_CURLY, unicode);
                n += glyph_fmt_into(dst, GLYPH_HLINE, unicode);
                n += glyph_fmt_into(dst, GLYPH_R_TEE, unicode);
                n += str_fmt_into(dst, NO_LIMIT, " %.*f ms\n", F64_PRECISION, elapsed);
                (*index)++;
                break;
            case TIMEIT_SPAN_END:
                return n;
            case TIMEIT_SPAN_TICK:
                if (*index > 0)
                    elapsed = ray_clock_elapsed_ms(&timeit->spans[*index - 1].clock, &span.clock);
                for (i = 0; i < indent; i++) {
                    n += glyph_fmt_into(dst, GLYPH_VLINE, unicode);
                    n += str_fmt_into(dst, 2, " ");
                }
                n += glyph_fmt_into(dst, GLYPH_ASTERISK, unicode);
                n += str_fmt_into(dst, NO_LIMIT, "  %s: %.*f ms\n", span.msg, F64_PRECISION, elapsed);
                (*index)++;
                break;
        }
    }

    return n;
}

obj_p timeit_fmt(nil_t) {
    timeit_t *timeit = &interpreter_current()->timeit;
    u64_t index = 0;
    obj_p dst = NULL_OBJ;

    timeit_fmt_into(&dst, 0, &index, timeit);

    return dst;
}

i64_t format_set_use_unicode(b8_t use) {
    __USE_UNICODE = use;
    return 0;
}

i64_t format_get_use_unicode() { return __USE_UNICODE; }

i64_t format_set_fpr(i64_t x) {
    F64_PRECISION = x;
    return 0;
}

i64_t format_set_display_width(i64_t x) {
    if (x < 1)
        MAX_ROW_WIDTH = DEFAULT_MAX_ROW_WIDTH;
    else
        MAX_ROW_WIDTH = x;

    return 0;
}
