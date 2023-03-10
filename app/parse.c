#include <stdio.h>
#include "parse.h"
#include "../core/storm.h"
#include "../core/alloc.h"
#include "../core/format.h"
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

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
    return c == '\n';
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
    i64_t *vec = (i64_t *)storm_malloc(cap * sizeof(i64_t));
    i64_t len = 0;
    str_t *current = &parser->current;
    value_t token;
    /*
     *type of vector:
     * 0 = initial
     * 1 = i64
     * 2 = f64
     * 3 = symbol
     */
    u8_t type = 0;

    (*current)++; // skip '['

    while (!at_eof(**current) && (**current) != ']')
    {
        token = advance(parser);

        if (is_error(&token))
            return token;

        // extend vec if needed
        if (len == cap)
        {
            cap *= 2;
            vec = storm_realloc(vec, cap * sizeof(i64_t));
        }

        if (token.type == -TYPE_I64)
        {
            if (type < 2)
            {
                vec[len++] = token.i64;
                type = 1;
            }
            else if (type == 2)
                ((f64_t *)vec)[len++] = (f64_t)token.i64;

            else
            {
                storm_free(vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else if (token.type == -TYPE_F64)
        {
            if (type == 2)
                ((f64_t *)vec)[len++] = token.f64;
            else if (type < 2)
            {
                for (i64_t i = 0; i < len; i++)
                    ((f64_t *)vec)[i] = (f64_t)vec[i];

                ((f64_t *)vec)[len++] = token.f64;

                type = 2;
            }
            else
            {
                storm_free(vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else if (token.type == -TYPE_SYMBOL)
        {
            if (type == 0 || type == 3)
            {
                vec[len++] = token.i64;
                type = 3;
            }
            else
            {
                storm_free(vec);
                return error(ERR_PARSE, "Invalid token in vector");
            }
        }
        else
        {
            storm_free(vec);
            return error(ERR_PARSE, "Invalid token in vector");
        }

        if ((**current) != ',')
            break;

        (*current)++;
    }

    if ((**current) != ']')
    {
        storm_free(vec);
        return error(ERR_PARSE, "Expected ']'");
    }

    (*current)++;

    switch (type)
    {
    case 1:
        return xi64(vec, len);
    case 2:
        return xf64((f64_t *)vec, len);
    case 3:
        return xsymbol(vec, len);
    };
}

value_t parse_symbol(parser_t *parser)
{
    str_t pos = parser->current;
    value_t res;

    // Skip first char and proceed until the end of the symbol
    do
    {
        pos++;
    } while (is_alphanum(*pos));

    res = symbol(parser->current, pos - parser->current);
    parser->current = pos;

    return res;
}

value_t advance(parser_t *parser)
{
    str_t *current = &parser->current;

    if (at_eof(**current))
        return s0(NULL, 0);

    while (is_whitespace(**current))
        (*current)++;

    if ((**current) == '[')
        return parse_vector(parser);

    if ((**current) == '-' || is_digit(**current))
        return parse_number(parser);

    if (is_alpha(**current))
        return parse_symbol(parser);

    return s0(NULL, 0);
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
        return error(ERR_PARSE, str_fmt("Unexpected token: %s", parser->current));

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
