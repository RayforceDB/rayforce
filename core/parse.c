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

null_t span_end(parser_t *parser, span_t *span)
{
    // span->line_end = parser->line;
    // span->col_end = parser->column;
}

rf_object_t label(span_t *span, str_t name)
{
    // rf_object_t l = dict(vector_symbol(0), list(0));
    // dict_set(&l, symbol("name"), string_from_str(name));
    // dict_set(&l, symbol("start_line"), i64(span->line_start));
    // dict_set(&l, symbol("start_col"), i64(span->col_start));
    // dict_set(&l, symbol("end_line"), i64(span->line_end));
    // dict_set(&l, symbol("end_col"), i64(span->col_end));
    // return l;
}

set_span(rf_object_t *token, span_t span)
{
    // token->span.start_line = span.line_start;
    // token->span.start_col = span.col_start;
    // token->span.end_line = span.line_end;
    // token->span.end_col = span.col_end;
}

null_t add_label(rf_object_t *error, span_t *span, str_t name)
{
    rf_object_t l = label(span, name);
    dict_set(error, symbol("labels"), l);
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

rf_object_t to_token(i8_t c)
{
    rf_object_t tok = i64(c);
    tok.type = TYPE_TOKEN;
    return tok;
}

rf_object_t parse_number(parser_t *parser)
{
    str_t end;
    i64_t num_i64;
    f64_t num_f64;
    rf_object_t num;

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
    {
        num = i64(num_i64);
    }

    shift(parser, end - parser->current);

    return num;
}

rf_object_t parse_string(parser_t *parser)
{
    parser->current++; // skip '"'
    str_t pos = parser->current;
    i32_t len;
    rf_object_t res;

    while (!at_eof(*pos))
    {

        if (*(pos - 1) == '\\' && *pos == '"')
            pos++;
        else if (*pos == '"')
            break;

        pos++;
    }

    if ((*pos) != '"')
        return error(ERR_PARSE, "Expected '\"'");

    len = pos - parser->current;
    res = string(len);
    strncpy(as_string(&res), parser->current, len);
    shift(parser, len + 1);

    return res;
}

rf_object_t parse_symbol(parser_t *parser)
{
    str_t pos = parser->current;
    rf_object_t res, s;
    i64_t id;

    // Skip first char and proceed until the end of the symbol
    do
    {
        pos++;
    } while (is_alphanum(*pos));

    s = str(parser->current, pos - parser->current);
    id = symbols_intern(&s);
    res = i64(id);
    res.type = -TYPE_SYMBOL;
    shift(parser, pos - parser->current);

    return res;
}

rf_object_t parse_vector(parser_t *parser)
{
    rf_object_t token, vec = vector_i64(0), err;
    i32_t i;

    // save current span
    span_t s = span_start(parser);

    shift(parser, 1); // skip '['
    token = advance(parser);

    while (!is_at(&token, ']'))
    {
        if (is_error(&token))
        {
            object_free(&vec);
            return token;
        }

        if (is_at(&token, '\0'))
        {
            object_free(&vec);
            span_end(parser, &s);
            err = error(ERR_PARSE, "Expected ']'");
            add_label(&err, &s, "started here");
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
                object_free(&vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else if (token.type == -TYPE_F64)
        {
            if (vec.type == TYPE_F64)
                vector_f64_push(&vec, token.f64);
            else if (vec.type == TYPE_I64)
            {

                for (i = 0; i < vec.adt.len; i++)
                    as_vector_f64(&vec)[i] = (f64_t)as_vector_i64(&vec)[i];

                vector_f64_push(&vec, token.f64);
                vec.type = TYPE_F64;
            }
            else
            {
                object_free(&vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else if (token.type == -TYPE_SYMBOL)
        {
            if (vec.type == TYPE_SYMBOL || (vec.adt.len == 0))
            {
                vector_i64_push(&vec, token.i64);
                vec.type = TYPE_SYMBOL;
            }
            else
            {
                object_free(&vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else
        {
            object_free(&vec);
            return error(ERR_PARSE, "Invalid token in vector");
        }

        token = advance(parser);
    }

    return vec;
}

rf_object_t parse_list(parser_t *parser)
{
    rf_object_t lst = list(0), token;

    shift(parser, 1); // skip '('
    token = advance(parser);

    while (!is_at(&token, ')'))
    {

        if (is_error(&token))
        {
            object_free(&lst);
            return token;
        }

        if (at_eof(*parser->current))
        {
            object_free(&lst);
            return error(ERR_PARSE, "Expected ')'");
        }

        if (is_at_term(&token))
        {
            object_free(&lst);
            return error(ERR_PARSE, str_fmt(0, "There is no opening found for: '%c'", token.i64));
        }

        list_push(&lst, token);

        token = advance(parser);
    }

    return lst;
}

rf_object_t parse_dict(parser_t *parser)
{
    rf_object_t token, keys = list(0), vals = list(0);

    shift(parser, 1); // skip '{'
    token = advance(parser);

    while (!is_at(&token, '}'))
    {
        if (is_error(&token))
        {
            object_free(&keys);
            object_free(&vals);
            return token;
        }

        if (at_eof(*parser->current))
        {
            object_free(&keys);
            object_free(&vals);
            return error(ERR_PARSE, "Expected '}'");
        }

        list_push(&keys, token);

        token = advance(parser);

        if (!is_at(&token, ':'))
        {
            object_free(&keys);
            object_free(&vals);
            return error(ERR_PARSE, "Expected ':'");
        }

        token = advance(parser);

        if (is_error(&token))
        {
            object_free(&keys);
            object_free(&vals);
            return token;
        }

        if (at_eof(*parser->current))
        {
            object_free(&keys);
            object_free(&vals);
            return error(ERR_PARSE, "Expected object");
        }

        list_push(&vals, token);

        token = advance(parser);
    }

    keys = list_flatten(keys);
    vals = list_flatten(vals);

    return dict(keys, vals);
}

rf_object_t advance(parser_t *parser)
{

    // Skip all whitespaces
    while (is_whitespace(*parser->current))
    {
        if (*parser->current == '\n')
        {
            parser->line++;
            parser->column = 0;
        }

        shift(parser, 1);
    }

    if (at_eof(*parser->current))
        return to_token(0);

    if ((*parser->current) == '[')
        return parse_vector(parser);

    if ((*parser->current) == '(')
        return parse_list(parser);

    if ((*parser->current) == '{')
        return parse_dict(parser);

    if ((*parser->current) == '-' || is_digit(*parser->current))
        return parse_number(parser);

    if (is_alpha(*parser->current))
        return parse_symbol(parser);

    if ((*parser->current) == '"')
        return parse_string(parser);

    if (at_term(*parser->current))
        return to_token(shift(parser, 1));

    return error(ERR_PARSE, str_fmt(0, "Unexpected token: '%s'", parser->current));
}

rf_object_t parse_program(parser_t *parser)
{
    str_t err_msg;
    rf_object_t token, list = list(0);

    while (!at_eof(*parser->current))
    {
        token = advance(parser);

        if (is_error(&token))
        {
            object_free(&list);
            return token;
        }

        if (is_at_term(&token))
        {
            object_free(&list);
            return error(ERR_PARSE, str_fmt(0, "There is no opening found for: '%c'", token.i64));
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
