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
#include "../core/rayforce.h"
#include "../core/alloc.h"
#include "../core/format.h"
#include "../core/string.h"
#include "../core/vector.h"
#include "../core/util.h"

#define TYPE_TOKEN 126

u8_t is_whitespace(i8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

u8_t is_digit(i8_t c)
{
    return c >= '0' && c <= '9';
}

u8_t is_alpha(i8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

u8_t is_alphanum(i8_t c)
{
    return is_alpha(c) || is_digit(c);
}

u8_t at_eof(i8_t c)
{
    return c == '\0' || c == EOF;
}

u8_t at_term(i8_t c)
{
    return c == ')' || c == ']' || c == '}' || c == ':' || c == '\0' || c == '\n';
}

u8_t is_at(value_t *token, i8_t c)
{
    // debug("is_at: %lld %d IS: %d\n", token->i64, c, token->type == TYPE_TOKEN && token->i64 == (i64_t)c);
    return token->type == TYPE_TOKEN && token->i64 == (i64_t)c;
}

u8_t is_at_term(value_t *token)
{
    return token->type == TYPE_TOKEN && at_term(token->i64);
}

u8_t shift(str_t *current)
{
    if (at_eof(**current))
        return '\0';

    u8_t res = **current;
    (*current)++;

    return res;
}

value_t to_token(i8_t c)
{
    value_t tok = i64(c);
    tok.type = TYPE_TOKEN;
    return tok;
}

value_t parse_number(parser_t *parser)
{
    str_t end;
    i64_t num_i64;
    f64_t num_f64;
    value_t num;
    str_t *current = &parser->current;

    errno = 0;

    num_i64 = strtoll(*current, &end, 10);

    if ((num_i64 == LONG_MAX || num_i64 == LONG_MIN) && errno == ERANGE)
        return error(ERR_PARSE, "Number out of range");

    if (end == *current)
        return error(ERR_PARSE, "Invalid number");

    // try double instead
    if (*end == '.')
    {
        num_f64 = strtod(*current, &end);

        if (errno == ERANGE)
            return error(ERR_PARSE, "Number out of range");

        num = f64(num_f64);
    }
    else
    {
        num = i64(num_i64);
    }

    *current = end;

    return num;
}

value_t parse_string(parser_t *parser)
{
    parser->current++; // skip '"'
    str_t pos = parser->current;
    u32_t len;
    value_t res;

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
    parser->current = pos + 1;

    return res;
}

value_t parse_symbol(parser_t *parser)
{
    str_t pos = parser->current;
    value_t res, s;
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
    parser->current = pos;

    return res;
}

value_t parse_vector(parser_t *parser)
{
    str_t *current = &parser->current;
    value_t token, vec = vector_i64(0);

    (*current)++; // skip '['
    token = advance(parser);

    while (!is_at(&token, ']'))
    {
        if (is_error(&token))
        {
            value_free(&vec);
            return token;
        }

        if (is_at(&token, '\0'))
        {
            value_free(&vec);
            return error(ERR_PARSE, "Expected ']'");
        }

        if (token.type == -TYPE_I64)
        {
            if (vec.type == TYPE_I64)
                vector_i64_push(&vec, token.i64);
            else if (vec.type == TYPE_F64)
                vector_f64_push(&vec, (f64_t)token.i64);
            else
            {
                value_free(&vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else if (token.type == -TYPE_F64)
        {
            if (vec.type == TYPE_F64)
                vector_f64_push(&vec, token.f64);
            else if (vec.type == TYPE_I64)
            {

                for (u64_t i = 0; i < vec.list.len; i++)
                    as_vector_f64(&vec)[i] = (f64_t)as_vector_i64(&vec)[i];

                vector_f64_push(&vec, token.f64);
                vec.type = TYPE_F64;
            }
            else
            {
                value_free(&vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else if (token.type == -TYPE_SYMBOL)
        {
            if (vec.type == TYPE_SYMBOL || (vec.list.len == 0))
            {
                vector_i64_push(&vec, token.i64);
                vec.type = TYPE_SYMBOL;
            }
            else
            {
                value_free(&vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else
        {
            value_free(&vec);
            return error(ERR_PARSE, "Invalid token in vector");
        }

        token = advance(parser);
    }

    return vec;
}

value_t parse_list(parser_t *parser)
{
    value_t lst = list(0), token;
    str_t *current = &parser->current;

    (*current)++; // skip '('
    token = advance(parser);

    while (!is_at(&token, ')'))
    {

        if (is_error(&token))
        {
            value_free(&lst);
            return token;
        }

        if (at_eof(**current))
        {
            value_free(&lst);
            return error(ERR_PARSE, "Expected ')'");
        }

        list_push(&lst, token);

        token = advance(parser);
    }

    return lst;
}

value_t parse_dict(parser_t *parser)
{
    str_t *current = &parser->current;
    value_t token, keys = list(0), vals = list(0);

    (*current)++; // skip '{'
    token = advance(parser);

    while (!is_at(&token, '}'))
    {
        if (is_error(&token))
        {
            value_free(&keys);
            value_free(&vals);
            return token;
        }

        if (at_eof(**current))
        {
            value_free(&keys);
            value_free(&vals);
            return error(ERR_PARSE, "Expected key");
        }

        list_push(&keys, token);

        token = advance(parser);

        if (!is_at(&token, ':'))
        {
            value_free(&keys);
            value_free(&vals);
            return error(ERR_PARSE, "Expected ':'");
        }

        token = advance(parser);

        if (is_error(&token))
        {
            value_free(&keys);
            value_free(&vals);
            return token;
        }

        if (at_eof(**current))
        {
            value_free(&keys);
            value_free(&vals);
            return error(ERR_PARSE, "Expected value");
        }

        list_push(&vals, token);

        token = advance(parser);
    }

    return dict(keys, vals);
}

value_t advance(parser_t *parser)
{
    str_t *current = &parser->current;

    if (at_eof(**current))
        return to_token(0);

    // Skip all whitespaces
    while (is_whitespace(**current))
        (*current)++;

    if ((**current) == '[')
        return parse_vector(parser);

    if ((**current) == '(')
        return parse_list(parser);

    if ((**current) == '{')
        return parse_dict(parser);

    if ((**current) == '-' || is_digit(**current))
        return parse_number(parser);

    if (is_alpha(**current))
        return parse_symbol(parser);

    if ((**current) == '"')
        return parse_string(parser);

    if (at_term(**current))
        return to_token(shift(current));

    return error(ERR_PARSE, str_fmt(0, "Unexpected token: %s", parser->current));
}

value_t parse_program(parser_t *parser)
{
    return advance(parser);

    str_t err_msg;
    value_t token, list = list(0);

    while (!at_eof(*parser->current))
    {
        token = advance(parser);

        if (is_error(&token))
        {
            err_msg = str_fmt(0, "%s:%d:%d: %s", parser->filename, parser->line, parser->column, as_string(&token));
            value_free(&token);
            return error(ERR_PARSE, err_msg);
        }

        if (is_at(&token, '\0'))
            break;

        if (is_at_term(&token))
        {
            err_msg = str_fmt(0, "%s:%d:%d: %s", parser->filename, parser->line, parser->column, "Unexpected token");
            value_free(&token);
            return error(ERR_PARSE, err_msg);
        }

        list_push(&list, token);
    }

    return list;
}

extern value_t parse(str_t filename, str_t input)
{
    value_t prg;

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
