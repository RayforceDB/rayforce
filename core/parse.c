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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include "parse.h"
#include "rayforce.h"
#include "heap.h"
#include "format.h"
#include "string.h"
#include "util.h"
#include "nfo.h"
#include "runtime.h"
#include "ops.h"
#include "eval.h"
#include "date.h"
#include "time.h"
#include "timestamp.h"
#include "error.h"

struct obj_t __PARSE_ADVANCE_OBJECT = {.type = TYPE_NULL};

#define PARSE_ADVANCE (&__PARSE_ADVANCE_OBJECT)

span_t span_start(parser_t *parser) {
    span_t s = {
        .start_line = (u16_t)parser->line,
        .end_line = (u16_t)parser->line,
        .start_column = (u16_t)parser->column,
        .end_column = (u16_t)parser->column,
    };

    return s;
}

nil_t span_extend(parser_t *parser, span_t *span) {
    span->end_line = parser->line;
    span->end_column = parser->column == 0 ? 0 : parser->column - 1;

    return;
}

obj_p parse_error(parser_t *parser, i64_t id, obj_p msg) {
    span_t span;
    obj_p err;

    err = error_obj(ERR_PARSE, msg);

    if (parser->nfo != NULL_OBJ) {
        span = nfo_get(parser->nfo, id);
        AS_ERROR(err)->locs = vn_list(1, vn_list(4,
                                                 i64(span.id),                        // span
                                                 clone_obj(AS_LIST(parser->nfo)[0]),  // file
                                                 NULL_OBJ,                            // function
                                                 clone_obj(AS_LIST(parser->nfo)[1])   // source
                                                 ));
    }

    return err;
}

b8_t is_whitespace(c8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0'; }

b8_t is_digit(c8_t c) { return c >= '0' && c <= '9'; }

b8_t is_alpha(c8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

b8_t is_alphanum(c8_t c) { return is_alpha(c) || is_digit(c); }

b8_t is_op(c8_t c) { return c && strchr("+-*/%&|^~<>!=._?", c) != NULL; }

b8_t at_eof(parser_t *parser) {
    return parser->current >= parser->input + parser->input_len || *parser->current == '\0';
}

b8_t before_eof(parser_t *parser) {
    return parser->current + 1 >= parser->input + parser->input_len || *parser->current == '\0';
}

b8_t at_term(c8_t c) {
    return c == ')' || c == ']' || c == '}' || c == ':' || c == ' ' || c == '\r' || c == '\n' || c == '\0';
}

b8_t is_at(obj_p token, c8_t c) { return token && token->type == TYPE_TOKEN && token->c8 == c; }

b8_t is_at_term(obj_p token) { return token && token->type == TYPE_TOKEN && at_term(token->c8); }

i8_t shift(parser_t *parser, i32_t num) {
    i8_t res;

    if (at_eof(parser))
        return 0;

    res = *parser->current;
    parser->current += num;
    parser->column += num;

    return res;
}

obj_p to_token(parser_t *parser) {
    obj_p tok;

    tok = c8(*parser->current);
    tok->type = TYPE_TOKEN;
    nfo_insert(parser->nfo, (i64_t)tok, span_start(parser));

    return tok;
}

obj_p parse_0Nx(parser_t *parser) {
    str_p current = parser->current;
    span_t span;
    obj_p res;

    if (*current == '0' && *(current + 1) == 'N') {
        span = span_start(parser);

        switch (*(current + 2)) {
            case '0':
                res = NULL_OBJ;
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'h':
                res = i16(NULL_I16);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'i':
                res = i32(NULL_I32);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'd':
                res = adate(NULL_I32);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 't':
                res = atime(NULL_I32);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'p':
                res = timestamp(NULL_I64);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'l':
                res = i64(NULL_I64);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'f':
                res = f64(NULL_F64);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 'g':
                res = guid(NULL_GUID);
                shift(parser, 3);
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
            case 's':
                shift(parser, 3);
                res = null(TYPE_SYMBOL);
                res->attrs = ATTR_QUOTED;
                nfo_insert(parser->nfo, (i64_t)res, span);
                return res;
        }
    }

    return PARSE_ADVANCE;
}

obj_p parse_time(parser_t *parser) {
    timestruct_t tm = {.null = B8_FALSE, .sign = 1, .hours = 0, .mins = 0, .secs = 0, .msecs = 0};
    str_p current = parser->current;
    span_t span = span_start(parser);

    if (*current == '-') {
        tm.sign = -1;
        current++;
    }

    if (is_digit(*current) && is_digit(*(current + 1))) {
        tm.hours = (*current - '0') * 10 + (*(current + 1) - '0');
        current += 2;
    } else
        return PARSE_ADVANCE;

    if (*current != ':')
        return PARSE_ADVANCE;

    current++;

    if (is_digit(*current) && is_digit(*(current + 1))) {
        tm.mins = (*current - '0') * 10 + (*(current + 1) - '0');
        current += 2;
    } else
        return PARSE_ADVANCE;

    if (*current != ':')
        return PARSE_ADVANCE;

    current++;

    if (is_digit(*current) && is_digit(*(current + 1))) {
        tm.secs = (*current - '0') * 10 + (*(current + 1) - '0');
        current += 2;
    } else
        return PARSE_ADVANCE;

    if (*current == '.') {
        current++;

        if (is_digit(*current) && is_digit(*(current + 1)) && is_digit(*(current + 2))) {
            tm.msecs = (*current - '0') * 100 + (*(current + 1) - '0') * 10 + (*(current + 2) - '0');
            current += 3;
        } else
            return PARSE_ADVANCE;
    }

    shift(parser, current - parser->current);
    span_extend(parser, &span);

    return atime(time_into_i32(tm));
}

obj_p parse_timestamp(parser_t *parser) {
    const char *end, *current = parser->current;
    u32_t nanos;
    datestruct_t dt = {.null = B8_FALSE, .year = 0, .month = 0, .day = 0};
    timestamp_t ts = {.null = B8_FALSE, .year = 0, .month = 0, .day = 0, .hours = 0, .mins = 0, .secs = 0, .nanos = 0};
    obj_p res;
    span_t span = span_start(parser);
    i64_t remaining = parser->input_len - (current - parser->input);

    // parse year
    if (remaining >= 4 && is_digit(*current) && is_digit(*(current + 1)) && is_digit(*(current + 2)) &&
        is_digit(*(current + 3))) {
        ts.year = (*current - '0') * 1000 + (*(current + 1) - '0') * 100 + (*(current + 2) - '0') * 10 +
                  (*(current + 3) - '0');
        current += 4;
    } else
        return PARSE_ADVANCE;

    // skip dot
    if (*current != '.')
        return PARSE_ADVANCE;

    current++;

    // parse month
    if (remaining >= 6 && is_digit(*current) && is_digit(*(current + 1))) {
        ts.month = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.month > 12) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Month is out of range"));
        }
    } else
        return PARSE_ADVANCE;

    // skip dot
    if (*current != '.')
        return PARSE_ADVANCE;

    current++;

    // parse day
    if (remaining >= 8 && is_digit(*current) && is_digit(*(current + 1))) {
        ts.day = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.day > 31) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Day is out of range"));
        }
    } else
        return NULL_OBJ;

    // just date passed
    if (*current != 'D') {
        shift(parser, current - parser->current);
        dt.year = ts.year;
        dt.month = ts.month;
        dt.day = ts.day;

        res = adate(date_into_i32(dt));
        span_extend(parser, &span);
        nfo_insert(parser->nfo, (i64_t)res, span);

        return res;
    }

    current++;

    // parse hour
    if (remaining >= 11 && is_digit(*current) && is_digit(*(current + 1))) {
        ts.hours = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.hours > 23) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Hour is out of range"));
        }
    } else
        return PARSE_ADVANCE;

    // skip colon
    if (*current != ':')
        return PARSE_ADVANCE;

    current++;

    // parse minute
    if (remaining >= 14 && is_digit(*current) && is_digit(*(current + 1))) {
        ts.mins = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.mins > 59) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Minute is out of range"));
        }
    } else
        return PARSE_ADVANCE;

    // skip colon
    if (*current != ':')
        return PARSE_ADVANCE;

    current++;

    // parse second
    if (remaining >= 17 && is_digit(*current) && is_digit(*(current + 1))) {
        ts.secs = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.secs > 59) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Second is out of range"));
        }
    } else
        return PARSE_ADVANCE;

    // skip dot
    if (*current != '.')
        return PARSE_ADVANCE;

    current++;

    // parse nanos
    nanos = strtoul(current, (char **)&end, 10);
    if (end > parser->input + parser->input_len) {
        end = parser->input + parser->input_len;
    }

    if (end == current)
        return PARSE_ADVANCE;

    ts.nanos = nanos;
    shift(parser, end - parser->current);
    res = timestamp(timestamp_into_i64(ts));

    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)res, span);

    return res;
}

obj_p specify_number(parser_t *parser, const char *current, span_t span, obj_p num) {
    obj_p res;

    // Check first
    switch (*current) {
        case 'x':
        case 'h':
        case 'i':
        case 'd':
        case 't':
        case 'l':
            if (num->type == -TYPE_F64) {
                current++;
                drop_obj(num);
                span.end_column += (current - parser->current);
                nfo_insert(parser->nfo, parser->count, span);
                return parse_error(parser, parser->count++,
                                   str_fmt(-1, "Invalid literal: integer can not be imaginary"));
            }
            break;
        default:
            break;
    }

    switch (*current++) {
        case 'x':
            if (num->i64 > 255) {
                drop_obj(num);
                span.end_column += (current - parser->current);
                nfo_insert(parser->nfo, parser->count, span);
                return parse_error(parser, parser->count++, str_fmt(-1, "Number is out of range"));
            }
            num->u8 = (u8_t)num->i64;
            num->type = -TYPE_U8;
            break;
        case 'h':
            if (num->i64 < -32767 || num->i64 > 32767) {
                drop_obj(num);
                span.end_column += (current - parser->current);
                nfo_insert(parser->nfo, parser->count, span);
                return parse_error(parser, parser->count++, str_fmt(-1, "Number is out of range"));
            }
            num->i16 = (i16_t)num->i64;
            num->type = -TYPE_I16;
            break;
        case 'i':
            if (num->i64 < -2147483647 || num->i64 > 2147483647) {
                drop_obj(num);
                span.end_column += (current - parser->current);
                nfo_insert(parser->nfo, parser->count, span);
                return parse_error(parser, parser->count++, str_fmt(-1, "Number is out of range"));
            }
            num->i32 = (i32_t)num->i64;
            num->type = -TYPE_I32;
            break;
        case 'd':
            num->type = -TYPE_DATE;
            break;
        case 't':
            num->type = -TYPE_TIME;
            break;
        case 'f':
            res = cast_obj(-TYPE_F64, num);
            drop_obj(num);
            num = res;
            break;
        case 'l':
            num->type = -TYPE_I64;
            break;
        default:
            current--;
            break;
    }

    shift(parser, current - parser->current);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)num, span);

    return num;
}

obj_p parse_number(parser_t *parser) {
    u8_t num_u8;
    i64_t r, num_i64;
    f64_t num_f64;
    obj_p num;
    span_t span = span_start(parser);
    i64_t remaining;

    remaining = parser->input_len - (parser->current - parser->input);

    if (remaining >= 2 && *parser->current == '0' && (*(parser->current + 1) == 'x')) {
        r = u8_from_str(parser->current + 2, remaining - 2, &num_u8);
        shift(parser, r + 2);
        span_extend(parser, &span);
        num = u8(num_u8);
        nfo_insert(parser->nfo, (i64_t)num, span);
        return num;
    }

    r = i64_from_str(parser->current, remaining, &num_i64);
    // if we parsed a number, and the next character is not a dot, then we can specify the number
    if (r > 0 && *(parser->current + r) != '.')
        return specify_number(parser, parser->current + r, span, i64(num_i64));

    // try f64 instead
    r = f64_from_str(parser->current, remaining, &num_f64);
    if (r > 0)
        return specify_number(parser, parser->current + r, span, f64(num_f64));

    span.end_column += (parser->current + remaining - parser->input);
    nfo_insert(parser->nfo, parser->count, span);
    return parse_error(parser, parser->count++, str_fmt(-1, "Not a number"));
}

obj_p parse_char(parser_t *parser) {
    span_t span = span_start(parser);
    str_p pos = parser->current + 1;  // skip '''
    obj_p res = NULL_OBJ;
    i64_t i, id;
    c8_t ch;

    // Handle empty quoted symbol (single quote)
    if (before_eof(parser) || at_term(*pos)) {
        // Return a null symbol (0Ns)
        shift(parser, 1);  // Skip the opening quote
        span_extend(parser, &span);
        res = symboli64(NULL_I64);  // Create a null symbol (NULL_I64) instead of 0
        res->attrs = ATTR_QUOTED;   // Make sure it's marked as quoted
        nfo_insert(parser->nfo, (i64_t)res, span);
        return res;
    }

    ch = '\0';

    // Check for escape sequences
    if (*pos == '\\') {
        pos++;  // Skip backslash
        switch (*pos) {
            case 'n':
                ch = '\n';
                pos++;
                break;
            case 'r':
                ch = '\r';
                pos++;
                break;
            case 't':
                ch = '\t';
                pos++;
                break;
            case '\\':
                ch = '\\';
                pos++;
                break;
            case '\'':
                ch = '\'';
                pos++;
                break;
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
                // Parse octal escape sequence \xxx
                ch = *pos - '0';
                pos++;
                for (i = 0; i < 2; i++) {
                    if (at_eof(parser) || !is_digit(*pos) || *pos > '7' || *pos < '0') {
                        span.end_column += (pos - parser->current);
                        nfo_insert(parser->nfo, parser->count, span);
                        return parse_error(parser, parser->count++, str_fmt(-1, "Invalid octal escape sequence"));
                    }
                    ch = (ch << 3) | (*pos - '0');
                    pos++;
                }
                break;
            default:
                span.end_column += (pos - parser->current);
                nfo_insert(parser->nfo, parser->count, span);
                return parse_error(parser, parser->count++, str_fmt(-1, "Invalid escape sequence"));
        }
    } else {
        ch = *pos;
        pos++;
    }

    // Check if the character is followed by a closing quote
    if (*pos == '\'') {
        pos++;
        res = c8(ch);
        shift(parser, pos - parser->current);  // Skip quotes, backslash, and escape sequence
        span_extend(parser, &span);
        nfo_insert(parser->nfo, (i64_t)res, span);
        return res;
    }

    // If not a char literal, parse as quoted symbol
    for (; (pos < parser->input + parser->input_len) && (is_alphanum(*pos) || is_op(*pos)); pos++)
        ;

    if (*pos == '\'') {
        span.end_column += (pos - parser->current);
        nfo_insert(parser->nfo, parser->count, span);
        return parse_error(parser, parser->count++, str_fmt(-1, "Char literal is too long"));
    }

    id = symbols_intern(parser->current + 1, pos - (parser->current + 1));
    res = i64(id);
    res->type = -TYPE_SYMBOL;
    res->attrs = ATTR_QUOTED;
    shift(parser, pos - parser->current);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)res, span);

    return res;
}

obj_p parse_string(parser_t *parser) {
    span_t span = span_start(parser);
    str_p pos = parser->current + 1;  // skip '"'
    i32_t i, len = 0;
    obj_p str, err;
    c8_t lf = '\n', cr = '\r', tb = '\t', bs = '\\', octal_val;

    str = C8(0);

    while (!at_eof(parser) && *pos != '\n') {
        if (*pos == '\\') {
            switch (*++pos) {
                case '\\':
                    push_raw(&str, &bs);
                    pos++;
                    break;
                case '"':
                    push_raw(&str, pos++);
                    break;
                case 'n':
                    push_raw(&str, &lf);
                    pos++;
                    break;
                case 'r':
                    push_raw(&str, &cr);
                    pos++;
                    break;
                case 't':
                    push_raw(&str, &tb);
                    pos++;
                    break;
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                    // Parse octal escape sequence \xxx
                    for (i = 0; i < 3; i++) {
                        if (at_eof(parser) || !is_digit(*pos) || *pos > '7' || *pos < '0') {
                            drop_obj(str);
                            span.end_column += (pos - parser->current);
                            nfo_insert(parser->nfo, parser->count, span);
                            return parse_error(parser, parser->count++, str_fmt(-1, "Invalid octal escape sequence"));
                        }

                        octal_val = (octal_val << 3) | (*pos - '0');
                        pos++;
                    }
                    push_raw(&str, &octal_val);
                    break;
                default:
                    drop_obj(str);
                    span.end_column += (pos - parser->current);
                    nfo_insert(parser->nfo, parser->count, span);
                    return parse_error(parser, parser->count++, str_fmt(-1, "Invalid escape sequence"));
            }

            continue;
        } else if (*pos == '"')
            break;

        push_raw(&str, pos++);
    }

    if ((*pos++) != '"') {
        drop_obj(str);
        span.end_column += (pos - parser->current);
        nfo_insert(parser->nfo, parser->count, span);
        err = parse_error(parser, parser->count++, str_fmt(-1, "Expected '\"'"));

        return err;
    }

    len = pos - parser->current - 2;

    shift(parser, len + 2);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)str, span);

    return str;
}

obj_p parse_symbol(parser_t *parser) {
    str_p pos = parser->current;
    obj_p res = NULL_OBJ;
    span_t span = span_start(parser);
    i64_t id;
    i64_t remaining = parser->input_len - (pos - parser->input);

    if (remaining >= 4 && strncmp(pos, "true", 4) == 0) {
        shift(parser, 4);
        span_extend(parser, &span);
        res = b8(B8_TRUE);
        nfo_insert(parser->nfo, (i64_t)res, span);
        return res;
    }

    if (remaining >= 5 && strncmp(pos, "false", 5) == 0) {
        shift(parser, 5);
        span_extend(parser, &span);
        res = b8(B8_FALSE);
        nfo_insert(parser->nfo, (i64_t)res, span);
        return res;
    }

    if (remaining >= 4 && strncmp(pos, "null", 4) == 0) {
        shift(parser, 4);
        span_extend(parser, &span);
        res = NULL_OBJ;
        nfo_insert(parser->nfo, (i64_t)res, span);
        return res;
    }

    // Skip first char and proceed until the end of the symbol
    do {
        pos++;
    } while (pos < parser->input + parser->input_len && (is_alphanum(*pos) || is_op(*pos)));

    id = symbols_intern(parser->current, pos - parser->current);

    if (parser->replace_symbols)
        res = env_get_internal_function_by_id(id);

    if (res == NULL_OBJ)
        res = symboli64(id);

    shift(parser, pos - parser->current);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)res, span);

    return res;
}

obj_p parse_vector(parser_t *parser) {
    obj_p tok, vec = I64(0), err;
    i64_t i;
    f64_t v;
    span_t span = span_start(parser);

    shift(parser, 1);  // skip '['
    parser->replace_symbols = B8_FALSE;
    tok = parser_advance(parser);
    parser->replace_symbols = B8_TRUE;

    while (!is_at(tok, ']')) {
        if (IS_ERR(tok)) {
            drop_obj(vec);
            return tok;
        }

        if (is_at(tok, '\0') || is_at_term(tok)) {
            err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Expected ']'"));
            drop_obj(vec);
            drop_obj(tok);
            return err;
        }

        if (tok->type == -TYPE_B8) {
            if (vec->len > 0 && vec->type != TYPE_B8) {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
            vec->type = TYPE_B8;
            push_raw(&vec, &tok->b8);
        } else if (tok->type == -TYPE_U8) {
            if (vec->len > 0 && vec->type != TYPE_U8) {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
            vec->type = TYPE_U8;
            push_raw(&vec, &tok->u8);
        } else if (tok->type == -TYPE_I16) {
            if (vec->type == TYPE_I16 || (vec->len == 0)) {
                push_raw(&vec, &tok->i16);
                vec->type = TYPE_I16;
            } else if (vec->type == TYPE_F64) {
                v = (f64_t)tok->i16;
                push_raw(&vec, &v);
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_I32) {
            if (vec->type == TYPE_I32 || (vec->len == 0)) {
                push_raw(&vec, &tok->i32);
                vec->type = TYPE_I32;
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_DATE) {
            if (vec->type == TYPE_DATE || (vec->len == 0)) {
                push_raw(&vec, &tok->i32);
                vec->type = TYPE_DATE;
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_TIME) {
            if (vec->type == TYPE_TIME || (vec->len == 0)) {
                push_raw(&vec, &tok->i32);
                vec->type = TYPE_TIME;
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_I64) {
            if (vec->type == TYPE_I64)
                push_raw(&vec, &tok->i64);
            else if (vec->type == TYPE_F64) {
                v = (f64_t)tok->i64;
                push_raw(&vec, &v);
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_F64) {
            if (vec->type == TYPE_F64)
                push_raw(&vec, &tok->f64);
            else if (vec->type == TYPE_I64) {
                vec->type = TYPE_F64;
                for (i = 0; i < vec->len; i++)
                    AS_F64(vec)[i] = (f64_t)AS_I64(vec)[i];

                push_raw(&vec, &tok->f64);
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_SYMBOL) {
            if (vec->type == TYPE_SYMBOL || (vec->len == 0)) {
                vec->type = TYPE_SYMBOL;
                push_raw(&vec, &tok->i64);
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else if (tok->type == -TYPE_TIMESTAMP) {
            if (vec->type == TYPE_TIMESTAMP || (vec->len == 0)) {
                push_raw(&vec, &tok->i64);
                vec->type = TYPE_TIMESTAMP;
            } else {
                err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
                drop_obj(vec);
                drop_obj(tok);
                return err;
            }
        } else {
            err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Invalid token in vector"));
            drop_obj(vec);
            drop_obj(tok);
            return err;
        }

        drop_obj(tok);
        span_extend(parser, &span);
        parser->replace_symbols = B8_FALSE;
        tok = parser_advance(parser);
        parser->replace_symbols = B8_TRUE;
    }

    drop_obj(tok);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)vec, span);

    return vec;
}

obj_p parse_list(parser_t *parser) {
    obj_p lst = NULL_OBJ, tok, args, body, err;
    span_t span = span_start(parser);

    shift(parser, 1);  // skip '('
    tok = parser_advance(parser);

    // parse lambda
    if (tok->type == -TYPE_SYMBOL && tok->i64 == SYMBOL_FN) {
        drop_obj(tok);

        args = parser_advance(parser);
        if (IS_ERR(args))
            return args;

        if (args->type != TYPE_SYMBOL) {
            if (args->type != TYPE_I64 || args->len != 0) {
                err = parse_error(parser, (i64_t)args, str_fmt(-1, "fn: expected type 'Symbol as arguments."));
                drop_obj(args);
                return err;
            }

            // empty args
            args->type = TYPE_SYMBOL;
        }

        body = parse_do(parser);
        if (IS_ERR(body)) {
            drop_obj(args);
            return body;
        }

        tok = parser_advance(parser);

        if (!is_at(tok, ')')) {
            span_extend(parser, &span);
            nfo_insert(parser->nfo, parser->count, span);
            err = parse_error(parser, parser->count++, str_fmt(-1, "fn: expected ')'"));
            drop_obj(args);
            drop_obj(body);
            drop_obj(tok);

            return err;
        }

        span_extend(parser, &span);
        lst = lambda(args, body, clone_obj(parser->nfo));
        nfo_insert(parser->nfo, (i64_t)lst, span);
        // insert self into nfo
        nfo_insert(AS_LAMBDA(lst)->nfo, (i64_t)lst, span);
        drop_obj(tok);

        return lst;
    }

    while (!is_at(tok, ')')) {
        if (IS_ERR(tok)) {
            drop_obj(lst);
            return tok;
        }

        if (at_eof(parser)) {
            nfo_insert(parser->nfo, parser->count, span);
            err = parse_error(parser, parser->count++, str_fmt(-1, "Expected ')'"));
            drop_obj(lst);
            drop_obj(tok);

            return err;
        }

        if (is_at_term(tok)) {
            err = parse_error(parser, (i64_t)tok, str_fmt(-1, "There is no opening found for: '%c'", tok->c8));
            drop_obj(lst);
            drop_obj(tok);

            return err;
        }

        if (lst == NULL_OBJ)
            lst = vn_list(1, tok);
        else
            push_obj(&lst, tok);

        span_extend(parser, &span);
        tok = parser_advance(parser);
    }

    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)lst, span);
    drop_obj(tok);

    return lst;
}

obj_p parse_dict(parser_t *parser) {
    obj_p tok, keys = NULL_OBJ, vals = LIST(0), d, err;
    span_t span = span_start(parser);

    shift(parser, 1);  // skip '{'
    parser->replace_symbols = B8_FALSE;
    tok = parser_advance(parser);
    parser->replace_symbols = B8_TRUE;

    while (!is_at(tok, '}')) {
        if (IS_ERR(tok)) {
            drop_obj(keys);
            drop_obj(vals);
            return tok;
        }

        if (at_eof(parser) || is_at_term(tok)) {
            nfo_insert(parser->nfo, parser->count, span);
            err = parse_error(parser, parser->count++, str_fmt(-1, "Expected '}'"));
            drop_obj(keys);
            drop_obj(vals);
            drop_obj(tok);

            return err;
        }

        if (keys == NULL_OBJ)
            keys = vector(tok->type, 0);

        push_obj(&keys, tok);

        span_extend(parser, &span);
        tok = parser_advance(parser);

        if (IS_ERR(tok)) {
            drop_obj(keys);
            drop_obj(vals);

            return tok;
        }

        if (!is_at(tok, ':')) {
            err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Expected ':'"));
            drop_obj(vals);
            drop_obj(keys);
            drop_obj(tok);

            return err;
        }

        span_extend(parser, &span);
        drop_obj(tok);
        tok = parser_advance(parser);

        if (IS_ERR(tok)) {
            drop_obj(keys);
            drop_obj(vals);

            return tok;
        }

        if (is_at_term(tok)) {
            nfo_insert(parser->nfo, parser->count, span);
            err = parse_error(parser, parser->count++, str_fmt(-1, "Expected value folowing ':'"));
            drop_obj(keys);
            drop_obj(vals);
            drop_obj(tok);

            return err;
        }

        push_obj(&vals, tok);

        span_extend(parser, &span);
        parser->replace_symbols = B8_FALSE;
        tok = parser_advance(parser);
        parser->replace_symbols = B8_TRUE;
    }

    drop_obj(tok);

    d = dict(keys, vals);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)d, span);

    return d;
}

nil_t skip_whitespaces(parser_t *parser) {
    while (!at_eof(parser)) {
        // Skip shebang
        if (*parser->current == '#' && *(parser->current + 1) == '!') {
            while (*parser->current != '\n' && !at_eof(parser))
                parser->current++;

            parser->line++;
            parser->column = 0;
        }

        // Handle whitespace characters
        if (is_whitespace(*parser->current)) {
            // Treat newline and carriage return as spaces
            if (*parser->current == '\n' || *parser->current == '\r') {
                parser->line++;
                parser->column = 0;
            } else {
                parser->column++;
            }
            parser->current++;
        }

        // Handle comments
        else if (*parser->current == ';') {
            while (*parser->current != '\n' && !at_eof(parser)) {
                parser->current++;
                parser->column++;
            }
        }

        else
            break;
    }
}

obj_p parser_advance(parser_t *parser) {
    obj_p tok = NULL_OBJ, err = NULL_OBJ;

    skip_whitespaces(parser);

    if (at_eof(parser))
        return to_token(parser);

    if (*parser->current == '[')
        return parse_vector(parser);

    if (*parser->current == '(')
        return parse_list(parser);

    if (*parser->current == '{')
        return parse_dict(parser);

    if (is_digit(*parser->current)) {
        tok = parse_0Nx(parser);
        if (tok != PARSE_ADVANCE)
            return tok;

        tok = parse_timestamp(parser);
        if (tok != PARSE_ADVANCE)
            return tok;

        drop_obj(tok);
    }

    if (((*parser->current) == '-' && is_digit(*(parser->current + 1))) || is_digit(*parser->current)) {
        tok = parse_time(parser);
        if (tok != PARSE_ADVANCE)
            return tok;

        tok = parse_number(parser);
        if (at_eof(parser))
            return tok;
        return tok;
    }

    if (*parser->current == '\'')
        return parse_char(parser);

    if (*parser->current == '"')
        return parse_string(parser);

    if (is_alpha(*parser->current) || is_op(*parser->current))
        return parse_symbol(parser);

    if (at_term(*parser->current)) {
        tok = to_token(parser);
        shift(parser, 1);
        return tok;
    }

    nfo_insert(parser->nfo, (i64_t)tok, span_start(parser));
    err = parse_error(parser, (i64_t)tok,
                      str_fmt(-1, "Unexpected token: '%c'", !at_eof(parser) ? *parser->current : '?'));
    drop_obj(tok);

    return err;
}

obj_p parse_do(parser_t *parser) {
    obj_p tok, car = PARSE_ADVANCE, lst = NULL_OBJ;

    while (!at_eof(parser)) {
        tok = parser_advance(parser);

        if (IS_ERR(tok)) {
            if (lst == NULL_OBJ) {
                // Only free car if it's not part of a list
                drop_obj(car);
            } else {
                // If we have a list, just free the list (which will free car)
                drop_obj(lst);
            }
            return tok;
        }

        if (is_at_term(tok)) {
            drop_obj(tok);
            // roll back one token
            parser->current--;
            parser->column--;
            break;
        }

        if (car == PARSE_ADVANCE)
            car = tok;
        else if (lst == NULL_OBJ)
            lst = vn_list(3, env_get_internal_function_by_id(SYMBOL_DO), car, tok);
        else
            push_obj(&lst, tok);
    }

    return lst != NULL_OBJ ? lst : car;
}

obj_p parse(lit_p input, i64_t input_len, obj_p nfo) {
    obj_p res, err;
    span_t span;

    parser_t parser = {
        .nfo = nfo,
        .count = 0,
        .input = input,
        .current = (str_p)input,
        .input_len = input_len,
        .line = 0,
        .column = 0,
        .replace_symbols = B8_TRUE,
    };

    res = parse_do(&parser);

    if (IS_ERR(res))
        return res;

    // Skip any remaining whitespace before checking for unparsed input
    skip_whitespaces(&parser);

    if (!at_eof(&parser)) {
        span = nfo_get(parser.nfo, (i64_t)res);
        span.start_column = span.end_column + 1;
        span.end_column = span.start_column;
        nfo_insert(parser.nfo, parser.count, span);
        err = parse_error(&parser, parser.count++, str_fmt(-1, "Unparsed input remains"));
        drop_obj(res);
        return err;
    }

    return res;
}
