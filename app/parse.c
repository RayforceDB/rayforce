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
#include "../core/bitspire.h"
#include "../core/alloc.h"
#include "../core/format.h"
#include "../core/string.h"

u8_t is_whitespace(u8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

u8_t is_digit(u8_t c)
{
    return c >= '0' && c <= '9';
}

u8_t is_alpha(u8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

u8_t is_alphanum(u8_t c)
{
    return is_alpha(c) || is_digit(c);
}

u8_t at_eof(u8_t c)
{
    return c == '\0' || c == '\n';
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

value_t parse_vector(parser_t *parser)
{
    i64_t cap = 8;
    str_t *current = &parser->current;
    value_t token, vec = vector_i64(0);

    (*current)++; // skip '['

    while (!at_eof(**current) && (**current) != ']')
    {
        token = advance(parser);

        if (is_error(&token))
        {
            value_free(&vec);
            return token;
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
    }

    if ((**current) != ']')
    {
        value_free(&vec);
        return error(ERR_PARSE, "Expected ']'");
    }

    (*current)++;

    return vec;
}

value_t parse_list(parser_t *parser)
{
    u64_t cap = 8;
    value_t vec = list(0), token;
    str_t *current = &parser->current;

    (*current)++; // skip '('

    while (!at_eof(**current) && (**current) != ')')
    {
        token = advance(parser);

        if (is_error(&token))
        {
            value_free(&vec);
            return token;
        }

        list_push(&vec, token);
    }

    if ((**current) != ')')
    {
        value_free(&vec);
        return error(ERR_PARSE, "Expected ')'");
    }

    (*current)++;

    return vec;
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

value_t parse_string(parser_t *parser)
{
    parser->current++; // skip '"'
    str_t pos = parser->current, new_str;
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

// value_t parse_dict(parser_t *parser)
// {
//     i64_t cap = 8;
//     u32_t len = 0;
//     str_t *current = &parser->current;
//     value_t *vec = (value_t *)bitspire_malloc(cap * sizeof(value_t));
//     value_t token;

//     (*current)++; // skip '{'

//     while (!at_eof(**current) && (**current) != '}')
//     {
//         token = advance(parser);

//         if (is_error(&token))
//         {
//             bitspire_free(vec);
//             return token;
//         }

//         // extend vec if needed
//         if (len == cap)
//         {
//             cap *= 2;
//             vec = bitspire_realloc(vec, cap * sizeof(value_t));
//         }

//         vec[len++] = token;
//     }

//     if ((**current) != '}')
//     {
//         bitspire_free(vec);
//         return error(ERR_PARSE, "Expected '}'");
//     }

//     (*current)++;

//     return dict(vec, len);
// }

value_t advance(parser_t *parser)
{
    str_t *current = &parser->current;

    if (at_eof(**current))
        return null();

    while (is_whitespace(**current))
        (*current)++;

    if ((**current) == '[')
        return parse_vector(parser);

    if ((**current) == '(')
        return parse_list(parser);

    if ((**current) == '-' || is_digit(**current))
        return parse_number(parser);

    if (is_alpha(**current))
        return parse_symbol(parser);

    if ((**current) == '"')
        return parse_string(parser);

    return null();
}

value_t parse_program(parser_t *parser)
{
    str_t err_msg;
    value_t token;

    // do
    // {
    token = advance(parser);
    // } while (token != NULL);

    if (!is_error(&token) && !at_eof(*parser->current))
        return error(ERR_PARSE, str_fmt(0, "Unexpected token: %s", parser->current));

    return token;
}

extern value_t parse(str_t filename, str_t input)
{
    value_t result_t;

    parser_t parser = {
        .filename = filename,
        .input = input,
        .current = input,
        .line = 0,
        .column = 0,
    };

    result_t = parse_program(&parser);

    return result_t;
}
