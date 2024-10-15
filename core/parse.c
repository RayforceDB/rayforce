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
#include "timestamp.h"
#include "error.h"

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

b8_t is_whitespace(c8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

b8_t IS_DIGIT(c8_t c) { return c >= '0' && c <= '9'; }

b8_t is_alpha(c8_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

b8_t is_alphanum(c8_t c) { return is_alpha(c) || IS_DIGIT(c); }

b8_t is_op(c8_t c) { return c && strchr("+-*/%&|^~<>!=._?", c) != NULL; }

b8_t at_eof(c8_t c) { return c == '\0' || (signed char)c == EOF; }

b8_t at_term(c8_t c) { return c == ')' || c == ']' || c == '}' || c == ':' || c == ' ' || c == '\r' || c == '\n'; }

b8_t is_at(obj_p token, c8_t c) { return token && token->type == -TYPE_C8 && token->c8 == c; }

b8_t is_at_term(obj_p token) { return token && token->type == -TYPE_C8 && at_term(token->c8); }

i8_t shift(parser_t *parser, i32_t num) {
    i8_t res;

    if (at_eof(*parser->current))
        return 0;

    res = *parser->current;
    parser->current += num;
    parser->column += num;

    return res;
}

obj_p to_token(parser_t *parser) {
    obj_p tok = c8(*parser->current);
    tok->type = -TYPE_C8;
    nfo_insert(parser->nfo, (i64_t)tok, span_start(parser));

    return tok;
}

obj_p parse_0x(parser_t *parser) {
    str_p end, current = parser->current;
    span_t span;
    u64_t num_u64;
    u8_t num_u8;
    obj_p res;

    if (*current == '0') {
        span = span_start(parser);

        if (*(current + 1) == 't') {
            res = timestamp(NULL_I64);
            shift(parser, 2);
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }

        if (*(parser->current + 1) == 'i') {
            res = i64(NULL_I64);
            shift(parser, 2);
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }

        if (*(parser->current + 1) == 'f') {
            res = f64(NULL_F64);
            shift(parser, 2);
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }

        if (*(parser->current + 1) == 'g') {
            res = guid(NULL_GUID);
            shift(parser, 2);
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }

        if (*(parser->current + 1) == 's') {
            shift(parser, 2);
            res = null(TYPE_SYMBOL);
            res->attrs = ATTR_QUOTED;
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }

        if (*(parser->current + 1) == 'x') {
            num_u64 = strtoul(parser->current, &end, 16);
            if (num_u64 > 255) {
                span.end_column += (end - parser->current);
                nfo_insert(parser->nfo, parser->count, span);
                return parse_error(parser, parser->count++, str_fmt(-1, "Number is out of range"));
            }
            num_u8 = (u8_t)num_u64;
            shift(parser, end - parser->current);
            span_extend(parser, &span);
            res = u8(num_u8);
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }
    }

    return NULL_OBJ;
}

obj_p parse_timestamp(parser_t *parser) {
    str_p end, current = parser->current;
    u32_t nanos;
    timestamp_t ts = {.null = B8_FALSE, .year = 0, .month = 0, .day = 0, .hours = 0, .mins = 0, .secs = 0, .nanos = 0};
    obj_p res;
    span_t span = span_start(parser);

    // parse year
    if (IS_DIGIT(*current) && IS_DIGIT(*(current + 1)) && IS_DIGIT(*(current + 2)) && IS_DIGIT(*(current + 3))) {
        ts.year = (*current - '0') * 1000 + (*(current + 1) - '0') * 100 + (*(current + 2) - '0') * 10 +
                  (*(current + 3) - '0');
        current += 4;
    } else
        return NULL_OBJ;

    // skip dot
    if (*current != '.')
        return NULL_OBJ;

    current++;

    // parse month
    if (IS_DIGIT(*current) && IS_DIGIT(*(current + 1))) {
        ts.month = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.month > 12) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Month is out of range"));
        }
    } else
        return NULL_OBJ;

    // skip dot
    if (*current != '.')
        return NULL_OBJ;

    current++;

    // parse day
    if (IS_DIGIT(*current) && IS_DIGIT(*(current + 1))) {
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
        res = timestamp(timestamp_into_i64(ts));

        span_extend(parser, &span);
        nfo_insert(parser->nfo, (i64_t)res, span);

        return res;
    }

    current++;

    // parse hour
    if (IS_DIGIT(*current) && IS_DIGIT(*(current + 1))) {
        ts.hours = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.hours > 23) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Hour is out of range"));
        }
    } else
        return NULL_OBJ;

    // skip colon
    if (*current != ':')
        return NULL_OBJ;

    current++;

    // parse minute
    if (IS_DIGIT(*current) && IS_DIGIT(*(current + 1))) {
        ts.mins = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.mins > 59) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Minute is out of range"));
        }
    } else
        return NULL_OBJ;

    // skip colon
    if (*current != ':')
        return NULL_OBJ;

    current++;

    // parse second
    if (IS_DIGIT(*current) && IS_DIGIT(*(current + 1))) {
        ts.secs = (*current - '0') * 10 + (*(current + 1) - '0');

        current += 2;

        if (ts.secs > 59) {
            span.start_column = current - parser->current - 2;
            span.end_column += current - parser->current - 1;
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Second is out of range"));
        }
    } else
        return NULL_OBJ;

    // skip dot
    if (*current != '.')
        return NULL_OBJ;

    current++;

    // parse nanos
    nanos = strtoul(current, &end, 10);

    if (end == current)
        return NULL_OBJ;

    ts.nanos = nanos;
    shift(parser, end - parser->current);
    res = timestamp(timestamp_into_i64(ts));

    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)res, span);

    return res;
}

obj_p parse_number(parser_t *parser) {
    str_p end;
    i64_t num_i64;
    f64_t num_f64;
    obj_p num;
    span_t span = span_start(parser);

    errno = 0;

    num_i64 = strtoll(parser->current, &end, 10);

    if ((num_i64 == LONG_MAX || num_i64 == LONG_MIN) && errno == ERANGE) {
        span.end_column += (end - parser->current - 1);
        nfo_insert(parser->nfo, parser->count, span);
        return parse_error(parser, parser->count++, str_fmt(-1, "Number is out of range"));
    }

    if (end == parser->current)
        return error_str(ERR_PARSE, "Invalid number");

    // try double instead
    if (*end == '.') {
        num_f64 = strtod(parser->current, &end);

        if (errno == ERANGE) {
            span.end_column += (end - parser->current);
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++, str_fmt(-1, "Number is out of range"));
        }

        num = f64(num_f64);
    } else
        num = i64(num_i64);

    shift(parser, end - parser->current);
    span_extend(parser, &span);
    nfo_insert(parser->nfo, (i64_t)num, span);

    return num;
}

obj_p parse_char(parser_t *parser) {
    span_t span = span_start(parser);
    str_p pos = parser->current + 1;  // skip '''
    obj_p res = NULL_OBJ;
    i64_t id;
    c8_t ch;

    if (at_eof(*pos) || at_term(*pos)) {
        shift(parser, pos - parser->current);
        span_extend(parser, &span);
        nfo_insert(parser->nfo, (i64_t)res, span);

        res = null(TYPE_SYMBOL);
        res->attrs = ATTR_QUOTED;

        return res;
    }

    if (*pos != '\'') {
        // char?
        if (*(pos + 1) == '\'') {
            ch = *pos++;
            res = c8(ch);

            shift(parser, 3);
            span_extend(parser, &span);
            nfo_insert(parser->nfo, (i64_t)res, span);

            return res;
        }

        // continue parsing a symbol
        while (is_alphanum(*pos) || is_op(*pos))
            pos++;

        if (*pos == '\'') {
            span.end_column += (pos - parser->current);
            nfo_insert(parser->nfo, parser->count, span);
            return parse_error(parser, parser->count++,
                               str_fmt(-1, "Invalid literal: char can not contain more than one symbol"));
        }
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

obj_p parse_C8(parser_t *parser) {
    span_t span = span_start(parser);
    str_p pos = parser->current + 1;  // skip '"'
    i32_t len = 0;
    obj_p str, err;
    c8_t lf = '\n', cr = '\r', tb = '\t';

    str = C8(0);

    while (!at_eof(*pos) && *pos != '\n') {
        if (*pos == '\\') {
            switch (*++pos) {
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

    if (strncmp(parser->current, "true", 4) == 0) {
        shift(parser, 4);
        span_extend(parser, &span);
        res = b8(B8_TRUE);
        nfo_insert(parser->nfo, (i64_t)res, span);

        return res;
    }

    if (strncmp(parser->current, "false", 5) == 0) {
        shift(parser, 5);
        span_extend(parser, &span);
        res = b8(B8_FALSE);
        nfo_insert(parser->nfo, (i64_t)res, span);

        return res;
    }

    if (strncmp(parser->current, "null", 4) == 0) {
        shift(parser, 4);
        span_extend(parser, &span);
        res = NULL_OBJ;
        nfo_insert(parser->nfo, (i64_t)res, span);

        return res;
    }

    // Skip first char and proceed until the end of the symbol
    do {
        pos++;
    } while (*pos && (is_alphanum(*pos) || is_op(*pos)));

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
    u64_t i;
    f64_t v;
    span_t span = span_start(parser);

    shift(parser, 1);  // skip '['
    parser->replace_symbols = B8_FALSE;
    tok = parser_advance(parser);
    parser->replace_symbols = B8_TRUE;

    while (!is_at(tok, ']')) {
        if (IS_ERROR(tok)) {
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
                    AS_F64(vec)
                [i] = (f64_t)AS_I64(vec)[i];

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

obj_p parse_LIST(parser_t *parser) {
    obj_p lst = NULL_OBJ, tok, args, body, err;
    span_t span = span_start(parser);

    shift(parser, 1);  // skip '('
    tok = parser_advance(parser);

    // parse lambda
    if (tok->type == -TYPE_SYMBOL && tok->i64 == SYMBOL_FN) {
        drop_obj(tok);

        args = parser_advance(parser);
        if (IS_ERROR(args))
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
        if (IS_ERROR(body)) {
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
        if (IS_ERROR(tok)) {
            drop_obj(lst);
            return tok;
        }

        if (at_eof(*parser->current)) {
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
        if (IS_ERROR(tok)) {
            drop_obj(keys);
            drop_obj(vals);
            return tok;
        }

        if (at_eof(*parser->current) || is_at_term(tok)) {
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

        if (IS_ERROR(tok)) {
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

        if (IS_ERROR(tok)) {
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
    while (B8_TRUE) {
        if (at_eof(*parser->current))
            break;

        // Skip shebang
        if (*parser->current == '#' && *(parser->current + 1) == '!') {
            while (*parser->current != '\n' && !at_eof(*parser->current))
                parser->current++;

            parser->line++;
            parser->column = 0;
        }

        // Handle whitespace characters
        if (is_whitespace(*parser->current)) {
            if (*parser->current == '\n') {
                parser->line++;
                parser->column = 0;
            } else
                parser->column++;

            parser->current++;
        }

        // Handle comments
        else if (*parser->current == ';') {
            while (*parser->current != '\n' && !at_eof(*parser->current)) {
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

    if (at_eof(*parser->current))
        return to_token(parser);

    if ((*parser->current) == '[')
        return parse_vector(parser);

    if ((*parser->current) == '(')
        return parse_LIST(parser);

    if ((*parser->current) == '{')
        return parse_dict(parser);

    if (IS_DIGIT(*parser->current)) {
        tok = parse_0x(parser);
        if (tok != NULL_OBJ)
            return tok;

        tok = parse_timestamp(parser);
        if (tok != NULL_OBJ)
            return tok;

        drop_obj(tok);
    }

    if (((*parser->current) == '-' && IS_DIGIT(*(parser->current + 1))) || IS_DIGIT(*parser->current))
        return parse_number(parser);

    if ((*parser->current) == '\'')
        return parse_char(parser);

    if ((*parser->current) == '"')
        return parse_C8(parser);

    if (is_alpha(*parser->current) || is_op(*parser->current))
        return parse_symbol(parser);

    if (at_term(*parser->current)) {
        tok = to_token(parser);
        shift(parser, 1);

        return tok;
    }

    nfo_insert(parser->nfo, (i64_t)tok, span_start(parser));
    err = parse_error(parser, (i64_t)tok, str_fmt(-1, "Unexpected token: '%c'", *parser->current));
    drop_obj(tok);

    return err;
}

obj_p parse_do(parser_t *parser) {
    obj_p tok, car = NULL_OBJ, lst = NULL_OBJ;

    while (!at_eof(*parser->current)) {
        tok = parser_advance(parser);

        if (IS_ERROR(tok)) {
            drop_obj(car);
            drop_obj(lst);
            return tok;
        }

        if (is_at(tok, '\0')) {
            drop_obj(tok);
            break;
        }

        if (is_at_term(tok)) {
            drop_obj(tok);
            // roll back one token
            parser->current--;
            parser->column--;
            break;
        }

        if (car == NULL_OBJ)
            car = tok;
        else if (lst == NULL_OBJ)
            lst = vn_list(3, env_get_internal_function_by_id(SYMBOL_DO), car, tok);
        else
            push_obj(&lst, tok);
    }

    return lst != NULL_OBJ ? lst : car;
}

obj_p parse(lit_p input, obj_p nfo) {
    obj_p res, err;
    span_t span;

    parser_t parser = {
        .nfo = nfo,
        .count = 0,
        .input = input,
        .current = (str_p)input,
        .line = 0,
        .column = 0,
        .replace_symbols = B8_TRUE,
    };

    res = parse_do(&parser);

    if (IS_ERROR(res))
        return res;

    if (!at_eof(*parser.current)) {
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
