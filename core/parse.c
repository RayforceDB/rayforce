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
#include "alloc.h"
#include "format.h"
#include "string.h"
#include "vector.h"
#include "util.h"
#include "dict.h"
#include "debuginfo.h"
#include "runtime.h"

#define TYPE_TOKEN 126

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

null_t span_extend(parser_t *parser, span_t *span)
{
    span->end_line = parser->line;
    span->end_column = parser->column == 0 ? 0 : parser->column - 1;

    return;
}

u32_t span_commit(span_t span)
{
    debuginfo_t *debuginfo = runtime_get()->debuginfo;
    return debuginfo_insert(debuginfo, span);
}

i8_t is_whitespace(i8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

i8_t is_digit(i8_t c)
{
    return c >= '0' && c <= '9';
}

i8_t is_alpha(i8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

i8_t is_alphanum(i8_t c)
{
    return is_alpha(c) || is_digit(c);
}

i8_t is_op(i8_t c)
{
    return strchr("+-*/%&|^~<>!=", c) != NULL;
}

i8_t at_eof(i8_t c)
{
    return c == '\0' || c == EOF;
}

i8_t at_term(i8_t c)
{
    return c == ')' || c == ']' || c == '}' || c == ':' || c == '\n';
}

i8_t is_at(rf_object_t *token, i8_t c)
{
    return token->type == TYPE_TOKEN && token->i64 == (i64_t)c;
}

i8_t is_at_term(rf_object_t *token)
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

rf_object_t to_token(parser_t *parser)
{
    rf_object_t tok = i64(*parser->current);
    tok.type = TYPE_TOKEN;
    tok.id = span_commit(span_start(parser));
    return tok;
}

rf_object_t parse_number(parser_t *parser)
{
    str_t end;
    i64_t num_i64;
    f64_t num_f64;
    rf_object_t num;
    span_t span = span_start(parser);

    // check if null
    if (*parser->current == '0')
    {
        if (*(parser->current + 1) == 'i')
        {
            shift(parser, 2);
            num = i64(NULL_I64);
            num.id = span_commit(span);
            return num;
        }

        if (*(parser->current + 1) == 'f')
        {
            shift(parser, 2);
            num = f64(NULL_F64);
            num.id = span_commit(span);
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
    num.id = span_commit(span);

    return num;
}

rf_object_t parse_string(parser_t *parser)
{
    span_t span = span_start(parser);
    str_t pos = parser->current + 1; // skip '"'
    i32_t len;
    rf_object_t str, err;

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
        err.id = span_commit(span);
        return err;
    }

    len = pos - parser->current - 2;
    str = string_from_str(parser->current + 1, len);

    shift(parser, len + 2);
    span_extend(parser, &span);
    str.id = span_commit(span);

    return str;
}

rf_object_t parse_symbol(parser_t *parser, i8_t quote)
{
    str_t pos = parser->current;
    rf_object_t res, s;
    span_t span = span_start(parser);

    // Skip first char and proceed until the end of the symbol
    do
    {
        pos++;
    } while (is_alphanum(*pos) || is_op(*pos));

    s = string_from_str(parser->current, pos - parser->current);
    res = symbol(as_string(&s));
    shift(parser, pos - parser->current);
    span_extend(parser, &span);
    res.id = span_commit(span);
    res.flags |= quote;

    return res;
}

rf_object_t parse_vector(parser_t *parser)
{
    rf_object_t token, vec = vector_i64(0), err;
    i32_t i;
    span_t span = span_start(parser);

    shift(parser, 1); // skip '['
    token = advance(parser);

    while (!is_at(&token, ']'))
    {
        if (is_error(&token))
        {
            rf_object_free(&vec);
            return token;
        }

        if (is_at(&token, '\0'))
        {
            rf_object_free(&vec);
            err = error(ERR_PARSE, "Expected ']'");
            err.id = span_commit(span);
            return err;
        }

        if (token.type == -TYPE_I64)
        {
            if (vec.type == TYPE_I64)
                vector_i64_push(&vec, token.i64);
            else if (vec.type == TYPE_F64)
                vector_f64_push(&vec, (f64_t)token.i64);
            else
            {
                rf_object_free(&vec);
                err = error(ERR_PARSE, "Invalid token in vector");
                err.id = token.id;
                return err;
            }
        }
        else if (token.type == -TYPE_F64)
        {
            if (vec.type == TYPE_F64)
                vector_f64_push(&vec, token.f64);
            else if (vec.type == TYPE_I64)
            {

                for (i = 0; i < vec.adt->len; i++)
                    as_vector_f64(&vec)[i] = (f64_t)as_vector_i64(&vec)[i];

                vector_f64_push(&vec, token.f64);
                vec.type = TYPE_F64;
            }
            else
            {
                rf_object_free(&vec);
                err = error(ERR_PARSE, "Invalid token in vector");
                err.id = token.id;
                return err;
            }
        }
        else if (token.type == -TYPE_SYMBOL)
        {
            if (vec.type == TYPE_SYMBOL || (vec.adt->len == 0))
            {
                vector_i64_push(&vec, token.i64);
                vec.type = TYPE_SYMBOL;
            }
            else
            {
                rf_object_free(&vec);
                err = error(ERR_PARSE, "Invalid token in vector");
                err.id = token.id;
                return err;
            }
        }
        else
        {
            rf_object_free(&vec);
            err = error(ERR_PARSE, "Invalid token in vector");
            err.id = token.id;
            return err;
        }

        span_extend(parser, &span);
        token = advance(parser);
    }

    span_extend(parser, &span);
    vec.id = span_commit(span);

    return vec;
}

rf_object_t parse_list(parser_t *parser, i8_t quote)
{
    rf_object_t lst = list(0), token, err;
    span_t span = span_start(parser);

    shift(parser, 1); // skip '('
    token = advance(parser);

    while (!is_at(&token, ')'))
    {

        if (is_error(&token))
        {
            rf_object_free(&lst);
            return token;
        }

        if (at_eof(*parser->current))
        {
            rf_object_free(&lst);
            err = error(ERR_PARSE, "Expected ')'");
            err.id = span_commit(span);
            return err;
        }

        if (is_at_term(&token))
        {
            rf_object_free(&lst);
            err = error(ERR_PARSE, str_fmt(0, "There is no opening found for: '%c'", token.i64));
            err.id = token.id;
            return err;
        }

        list_push(&lst, token);

        span_extend(parser, &span);
        token = advance(parser);
    }

    span_extend(parser, &span);
    lst.id = span_commit(span);
    lst.flags |= quote;

    return lst;
}

rf_object_t parse_dict(parser_t *parser)
{
    rf_object_t token, keys = list(0), vals = list(0), d, err;
    span_t span = span_start(parser);

    shift(parser, 1); // skip '{'
    token = advance(parser);

    while (!is_at(&token, '}'))
    {
        if (is_error(&token))
        {
            rf_object_free(&keys);
            rf_object_free(&vals);
            return token;
        }

        if (at_eof(*parser->current))
        {
            rf_object_free(&keys);
            rf_object_free(&vals);
            err = error(ERR_PARSE, "Expected '}'");
            err.id = span_commit(span);
            return err;
        }

        list_push(&keys, token);

        span_extend(parser, &span);
        token = advance(parser);

        if (!is_at(&token, ':'))
        {
            rf_object_free(&keys);
            rf_object_free(&vals);
            err = error(ERR_PARSE, "Expected ':'");
            err.id = token.id;
            return err;
        }

        token = advance(parser);

        if (is_error(&token))
        {
            rf_object_free(&keys);
            rf_object_free(&vals);
            return token;
        }

        if (at_eof(*parser->current))
        {
            rf_object_free(&keys);
            rf_object_free(&vals);
            err = error(ERR_PARSE, "Expected rf_object folowing ':'");
            err.id = span_commit(span);
            return err;
        }

        list_push(&vals, token);

        span_extend(parser, &span);
        token = advance(parser);
    }

    keys = list_flatten(keys);
    vals = list_flatten(vals);
    d = dict(keys, vals);

    span_extend(parser, &span);
    d.id = span_commit(span);

    return d;
}

rf_object_t advance(parser_t *parser)
{
    rf_object_t tok, err;
    i8_t quote = 0;

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

    if ((*parser->current) == '\'')
    {
        quote = 1;
        shift(parser, 1);
    }

    if (at_eof(*parser->current))
        return to_token(parser);

    if ((*parser->current) == '[')
        return parse_vector(parser);

    if ((*parser->current) == '(')
        return parse_list(parser, quote);

    if ((*parser->current) == '{')
        return parse_dict(parser);

    if (((*parser->current) == '-' && is_digit(*(parser->current + 1))) || is_digit(*parser->current))
        return parse_number(parser);

    if (is_alpha(*parser->current) || is_op(*parser->current))
        return parse_symbol(parser, quote);

    if ((*parser->current) == '"')
        return parse_string(parser);

    if (at_term(*parser->current))
    {
        tok = to_token(parser);
        shift(parser, 1);

        return tok;
    }

    err = error(ERR_PARSE, str_fmt(0, "Unexpected token: '%c'", *parser->current));
    err.id = span_commit(span_start(parser));
    return err;
}

rf_object_t parse_program(parser_t *parser)
{
    return advance(parser);
    rf_object_t token, list = list(0), err;

    while (!at_eof(*parser->current))
    {
        token = advance(parser);

        if (is_error(&token))
        {
            rf_object_free(&list);
            return token;
        }

        if (is_at_term(&token))
        {
            rf_object_free(&list);
            err = error(ERR_PARSE, str_fmt(0, "There is no opening found for: '%c'", token.i64));
            err.id = token.id;
            return err;
        }

        if (is_at(&token, '\0'))
            break;

        list_push(&list, token);
    }

    return list;
}

extern rf_object_t parse(str_t filename, str_t input)
{
    rf_object_t prg;

    parser_t parser = {
        .filename = filename,
        .input = input,
        .current = input,
        .line = 0,
        .column = 0,
    };

    prg = parse_program(&parser);

    return prg;
}
