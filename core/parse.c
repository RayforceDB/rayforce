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
#include "vector.h"
#include "util.h"
#include "dict.h"
#include "debuginfo.h"
#include "runtime.h"
#include "ops.h"
#include "timestamp.h"

span_t span_start(parser_t *parser)
{
    span_t s = {
        .start_line = parser->line,
        .end_line = parser->line,
        .start_column = parser->column,
        .end_column = parser->column,
    };

    return s;
}

nil_t span_extend(parser_t *parser, span_t *span)
{
    span->end_line = parser->line;
    span->end_column = parser->column == 0 ? 0 : parser->column - 1;

    return;
}

u32_t span_commit(parser_t *parser, span_t span)
{
    debuginfo_insert(&parser->debuginfo, parser->count, span);
    return parser->count++;
}

bool_t is_whitespace(i8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool_t is_digit(i8_t c)
{
    return c >= '0' && c <= '9';
}

bool_t is_alpha(i8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool_t is_alphanum(i8_t c)
{
    return is_alpha(c) || is_digit(c);
}

bool_t is_op(i8_t c)
{
    return strchr("+-*/%&|^~<>!=", c) != NULL;
}

bool_t at_eof(i8_t c)
{
    return c == '\0' || c == EOF;
}

bool_t at_term(i8_t c)
{
    return c == ')' || c == ']' || c == '}' || c == ':' || c == '\n';
}

bool_t is_at(obj_t token, i8_t c)
{
    return token->type == TYPE_TOKEN && token->i64 == (i64_t)c;
}

bool_t is_at_term(obj_t token)
{
    return token->type == TYPE_TOKEN && at_term(token->i64);
}

i8_t shift(parser_t *parser, i32_t num)
{
    if (at_eof(*parser->current))
        return 0;

    i8_t res = *parser->current;
    parser->current += num;
    parser->column += num;

    return res;
}

obj_t to_token(parser_t *parser)
{
    obj_t tok = i64(*parser->current);
    tok->type = TYPE_TOKEN;
    tok->id = span_commit(parser, span_start(parser));
    return tok;
}

obj_t parse_timestamp(parser_t *parser)
{
    str_t end, current = parser->current;
    u32_t nanos;
    timestamp_t ts = {0};
    obj_t res;
    span_t span = span_start(parser);

    // check if null
    if (*current == '0')
    {
        if (*(current + 1) == 't')
        {
            shift(parser, 2);
            res = timestamp(NULL_I64);
            res->id = span_commit(parser, span);
            return res;
        }
    }

    // parse year
    if (is_digit(*current) &&
        is_digit(*(current + 1)) &&
        is_digit(*(current + 2)) &&
        is_digit(*(current + 3)))
    {
        ts.year = (*current - '0') * 1000 +
                  (*(current + 1) - '0') * 100 +
                  (*(current + 2) - '0') * 10 +
                  (*(current + 3) - '0');
        current += 4;
    }
    else
        return null();

    // skip dot
    if (*current != '.')
        return null();

    current++;

    // parse month
    if (is_digit(*current) &&
        is_digit(*(current + 1)))
    {
        ts.month = (*current - '0') * 10 +
                   (*(current + 1) - '0');
        current += 2;
    }
    else
        return null();

    // skip dot
    if (*current != '.')
        return null();

    current++;

    // parse day
    if (is_digit(*current) &&
        is_digit(*(current + 1)))
    {
        ts.day = (*current - '0') * 10 +
                 (*(current + 1) - '0');
        current += 2;
    }
    else
        return null();

    // skip D
    if (*current != 'D')
        return null();

    current++;

    // parse hour
    if (is_digit(*current) &&
        is_digit(*(current + 1)))
    {
        ts.hours = (*current - '0') * 10 +
                   (*(current + 1) - '0');
        current += 2;
    }
    else
        return null();

    // skip colon
    if (*current != ':')
        return null();

    current++;

    // parse minute
    if (is_digit(*current) &&
        is_digit(*(current + 1)))
    {
        ts.mins = (*current - '0') * 10 +
                  (*(current + 1) - '0');
        current += 2;
    }
    else
        return null();

    // skip colon
    if (*current != ':')
        return null();

    current++;

    // parse second
    if (is_digit(*current) &&
        is_digit(*(current + 1)))
    {
        ts.secs = (*current - '0') * 10 +
                  (*(current + 1) - '0');
        current += 2;
    }
    else
        return null();

    // skip dot
    if (*current != '.')
        return null();

    current++;

    // parse nanos
    nanos = strtoul(current, &end, 10);

    if (end == current)
        return null();

    ts.nanos = nanos;
    shift(parser, end - parser->current);
    res = timestamp(rf_timestamp_into_i64(ts));

    span_extend(parser, &span);
    res->id = span_commit(parser, span);

    return res;
}

obj_t parse_number(parser_t *parser)
{
    str_t end;
    i64_t num_i64;
    f64_t num_f64;
    obj_t num;
    span_t span = span_start(parser);

    // check if null
    if (*parser->current == '0')
    {
        if (*(parser->current + 1) == 'i')
        {
            shift(parser, 2);
            num = i64(NULL_I64);
            num->id = span_commit(parser, span);
            return num;
        }

        if (*(parser->current + 1) == 'f')
        {
            shift(parser, 2);
            num = f64(NULL_F64);
            num->id = span_commit(parser, span);
            return num;
        }

        if (*(parser->current + 1) == 't')
        {
            shift(parser, 2);
            num = timestamp(NULL_I64);
            num->id = span_commit(parser, span);
            return num;
        }

        if (*(parser->current + 1) == 'g')
        {
            shift(parser, 2);
            num = guid(NULL);
            num->id = span_commit(parser, span);
            return num;
        }
    }

    errno = 0;

    num_i64 = strtoll(parser->current, &end, 10);

    if ((num_i64 == LONG_MAX || num_i64 == LONG_MIN) && errno == ERANGE)
        return error(ERR_PARSE, "Number out of range");

    if (end == parser->current)
        return error(ERR_PARSE, "Invalid number");

    // try double instead
    if (*end == '.')
    {
        num_f64 = strtod(parser->current, &end);

        if (errno == ERANGE)
            return error(ERR_PARSE, "Number out of range");

        num = f64(num_f64);
    }
    else
        num = i64(num_i64);

    shift(parser, end - parser->current);
    span_extend(parser, &span);
    num->id = span_commit(parser, span);

    return num;
}

obj_t parse_char(parser_t *parser)
{
    span_t span = span_start(parser);
    str_t pos = parser->current + 1; // skip '''
    obj_t ch, err;

    if (at_eof(*pos) || *pos == '\n')
    {
        span.end_column += (pos - parser->current);
        err = error(ERR_PARSE, "Expected character");
        err->id = span_commit(parser, span);
        return err;
    }

    ch = schar(*pos++);

    if (*pos != '\'')
    {
        span.end_column += (parser->current - parser->current);
        err = error(ERR_PARSE, "Expected '''");
        err->id = span_commit(parser, span);
        return err;
    }

    shift(parser, 3);
    span_extend(parser, &span);
    ch->id = span_commit(parser, span);

    return ch;
}

obj_t parse_string(parser_t *parser)
{
    span_t span = span_start(parser);
    str_t pos = parser->current + 1; // skip '"'
    i32_t len;
    obj_t str, err;

    while (!at_eof(*pos) && *pos != '\n')
    {

        if (*(pos - 1) == '\\' && *pos == '"')
            pos++;
        else if (*pos == '"')
            break;

        pos++;
    }

    if ((*pos++) != '"')
    {
        span.end_column += (pos - parser->current);
        err = error(ERR_PARSE, "Expected '\"'");
        err->id = span_commit(parser, span);
        return err;
    }

    len = pos - parser->current - 2;
    str = string_from_str(parser->current + 1, len);

    shift(parser, len + 2);
    span_extend(parser, &span);
    str->id = span_commit(parser, span);

    return str;
}

obj_t parse_symbol(parser_t *parser)
{
    str_t pos = parser->current;
    obj_t res;
    span_t span = span_start(parser);
    i64_t id;

    if (strncmp(parser->current, "true", 4) == 0)
    {
        shift(parser, 4);
        span_extend(parser, &span);
        res = bool(true);
        res->id = span_commit(parser, span);
        return res;
    }

    if (strncmp(parser->current, "false", 5) == 0)
    {
        shift(parser, 5);
        span_extend(parser, &span);
        res = bool(false);
        res->id = span_commit(parser, span);
        return res;
    }

    if (strncmp(parser->current, "null", 4) == 0)
    {
        shift(parser, 4);
        span_extend(parser, &span);
        res = null();
        res->id = span_commit(parser, span);
        return res;
    }

    // Skip first char and proceed until the end of the symbol
    do
    {
        pos++;
    } while (is_alphanum(*pos) || is_op(*pos));

    id = intern_symbol(parser->current, pos - parser->current);
    res = i64(id);
    res->type = -TYPE_SYMBOL;
    shift(parser, pos - parser->current);
    span_extend(parser, &span);
    res->id = span_commit(parser, span);

    return res;
}

obj_t parse_vector(parser_t *parser)
{
    obj_t token, vec = vector_i64(0), err;
    i32_t i;
    span_t span = span_start(parser);

    shift(parser, 1); // skip '['
    token = advance(parser);

    while (!is_at(token, ']'))
    {
        if (is_error(token))
        {
            drop(vec);
            return token;
        }

        if (is_at(token, '\0') || is_at_term(token))
        {
            drop(vec);
            drop(token);
            err = error(ERR_PARSE, "Expected ']'");
            err->id = token->id;
            return err;
        }

        if (token->type == -TYPE_BOOL)
        {
            if (vec->len > 0 && vec->type != TYPE_BOOL)
            {
                drop(vec);
                drop(token);
                err = error(ERR_PARSE, "Invalid token in vector");
                err->id = token->id;
                return err;
            }

            vec->type = TYPE_BOOL;
            join_raw(vec, token->bool);
        }
        else if (token->type == -TYPE_I64)
        {
            if (vec->type == TYPE_I64)
                join_raw(vec, token->i64);
            else if (vec->type == TYPE_F64)
                join_raw(vec, token->i64);
            else
            {
                drop(vec);
                drop(token);
                err = error(ERR_PARSE, "Invalid token in vector");
                err->id = token->id;
                return err;
            }
        }
        else if (token->type == -TYPE_F64)
        {
            if (vec->type == TYPE_F64)
                join_raw(vec, token);
            else if (vec->type == TYPE_I64)
            {
                vec->type = TYPE_F64;
                for (i = 0; i < (i32_t)vec->len; i++)
                    as_vector_f64(vec)[i] = (f64_t)as_vector_i64(vec)[i];

                join_raw(vec, (i64_t)token->f64);
            }
            else
            {
                drop(vec);
                drop(token);
                err = error(ERR_PARSE, "Invalid token in vector");
                err->id = token->id;
                return err;
            }
        }
        else if (token->type == -TYPE_SYMBOL)
        {
            if (vec->type == TYPE_SYMBOL || (vec->len == 0))
            {
                vec->type = TYPE_SYMBOL;
                join_raw(vec, token->i64);
            }
            else
            {
                drop(vec);
                err = error(ERR_PARSE, "Invalid token in vector");
                err->id = token->id;
                return err;
            }
        }
        else if (token->type == -TYPE_TIMESTAMP)
        {
            if (vec->type == TYPE_TIMESTAMP || (vec->len == 0))
            {
                join_raw(vec, token);
                vec->type = TYPE_TIMESTAMP;
            }
            else
            {
                drop(vec);
                err = error(ERR_PARSE, "Invalid token in vector");
                err->id = token->id;
                return err;
            }
        }
        else
        {
            drop(vec);
            err = error(ERR_PARSE, "Invalid token in vector");
            err->id = token->id;
            return err;
        }

        drop(token);
        span_extend(parser, &span);
        token = advance(parser);
    }

    span_extend(parser, &span);
    vec->id = span_commit(parser, span);

    return vec;
}

obj_t parse_list(parser_t *parser)
{
    obj_t lst = list(0), token, err;
    span_t span = span_start(parser);
    str_t msg;

    shift(parser, 1); // skip '('
    token = advance(parser);

    while (!is_at(token, ')'))
    {

        if (is_error(token))
        {
            drop(lst);
            return token;
        }

        if (at_eof(*parser->current))
        {
            drop(lst);
            drop(token);
            err = error(ERR_PARSE, "Expected ')'");
            err->id = span_commit(parser, span);
            return err;
        }

        if (is_at_term(&token))
        {
            drop(lst);
            drop(token);
            msg = str_fmt(0, "There is no opening found for: '%c'", token->i64);
            err = error(ERR_PARSE, msg);
            heap_free(msg);
            err->id = token->id;
            return err;
        }

        join_obj(lst, token);

        span_extend(parser, &span);
        token = advance(parser);
    }

    span_extend(parser, &span);
    lst->id = span_commit(parser, span);

    return lst;
}

obj_t parse_dict(parser_t *parser)
{
    obj_t token, keys = list(0), vals = list(0), d, err;
    span_t span = span_start(parser);

    shift(parser, 1); // skip '{'
    token = advance(parser);

    while (!is_at(token, '}'))
    {
        if (is_error(token))
        {
            drop(keys);
            drop(vals);
            return token;
        }

        if (at_eof(*parser->current) || is_at_term(token))
        {
            drop(keys);
            drop(vals);
            err = error(ERR_PARSE, "Expected '}'");
            err->id = token->id;
            return err;
        }

        join_obj(keys, token);

        span_extend(parser, &span);
        token = advance(parser);

        if (is_error(token))
        {
            drop(keys);
            drop(vals);
            return token;
        }

        if (!is_at(&token, ':'))
        {
            err = error(ERR_PARSE, "Expected ':'");
            err->id = token->id;
            drop(vals);
            drop(keys);
            drop(token);
            return err;
        }

        token = advance(parser);

        if (is_error(token))
        {
            drop(keys);
            drop(vals);
            return token;
        }

        if (at_eof(*parser->current) || is_at_term(token))
        {
            drop(keys);
            drop(vals);
            err = error(ERR_PARSE, "Expected value folowing ':'");
            err->id = token->id;
            return err;
        }

        join_obj(vals, token);

        span_extend(parser, &span);
        token = advance(parser);
    }

    d = dict(keys, vals);

    span_extend(parser, &span);
    d->id = span_commit(parser, span);

    return d;
}

obj_t advance(parser_t *parser)
{
    obj_t tok, err;
    str_t msg;

    // Skip all whitespaces
    while (is_whitespace(*parser->current))
    {
        // Update line and column (if next line is not empty)
        if (*parser->current == '\n')
        {
            parser->line++;
            parser->column = 0;
            parser->current++;
        }
        else
            shift(parser, 1);
    }

    // Skip comments
    if ((*parser->current) == ';')
    {
        while (!at_eof(*parser->current) && (*parser->current) != '\n')
            shift(parser, 1);

        return advance(parser);
    }

    if ((*parser->current) == '`')
    {
        shift(parser, 1);

        tok = advance(parser);
        if (is_error(tok))
            return tok;

        if (is_at_term(tok))
            tok = null();

        return list(2, symbol("`"), tok);
    }

    if (at_eof(*parser->current))
        return to_token(parser);

    if ((*parser->current) == '[')
        return parse_vector(parser);

    if ((*parser->current) == '(')
        return parse_list(parser);

    if ((*parser->current) == '{')
        return parse_dict(parser);

    if (is_digit(*parser->current))
    {
        tok = parse_timestamp(parser);
        if (!is_null(tok))
            return tok;
    }

    if (((*parser->current) == '-' && is_digit(*(parser->current + 1))) || is_digit(*parser->current))
        return parse_number(parser);

    if (is_alpha(*parser->current) || is_op(*parser->current))
        return parse_symbol(parser);

    if ((*parser->current) == '\'')
        return parse_char(parser);

    if ((*parser->current) == '"')
        return parse_string(parser);

    if (at_term(*parser->current))
    {
        tok = to_token(parser);
        shift(parser, 1);

        return tok;
    }

    msg = str_fmt(0, "Unexpected token: '%c'", *parser->current);
    err = error(ERR_PARSE, msg);
    err->id = span_commit(parser, span_start(parser));

    return err;
}

obj_t parse_program(parser_t *parser)
{
    obj_t token, lst = list(0), err;
    str_t msg;

    while (!at_eof(*parser->current))
    {
        token = advance(parser);

        if (is_error(token))
        {
            drop(list);
            return token;
        }

        if (is_at_term(token))
        {
            drop(lst);
            msg = str_fmt(0, "Unexpected token: '%c'", token->i64);
            err = error(ERR_PARSE, msg);
            heap_free(msg);
            err->id = token->id;
            return err;
        }

        if (is_at(token, '\0'))
            break;

        join_obj(lst, token);
    }

    return lst;
}

obj_t parse(parser_t *parser, str_t filename, str_t input)
{
    obj_t prg;

    parser->debuginfo.lambda = "";
    parser->debuginfo.filename = filename;
    parser->input = input;
    parser->current = input;
    parser->line = 0;
    parser->column = 0;

    prg = parse_program(parser);

    // if (is_error(prg))
    //     prg->span = debuginfo_get(&parser->debuginfo, prg->id );

    return prg;
}

parser_t parser_new()
{
    parser_t parser = {
        .debuginfo = debuginfo_new("", ""),
    };

    return parser;
}

void parser_free(parser_t *parser)
{
    debuginfo_free(&parser->debuginfo);
}