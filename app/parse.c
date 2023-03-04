
#include "lex.h"
#include "parse.h"
#include "../core/storm.h"
#include "../core/alloc.h"

parser_t new_parser(str_t filename, lexer_t lexer)
{
    parser_t parser;

    parser = (parser_t)storm_malloc(sizeof(struct parser_t));
    parser->filename = filename;
    parser->lexer = lexer;
    return parser;
}

value_t parse_program(parser_t parser)
{
    str_t err_msg;
    token_t token;

    do
    {
        token = next_token(parser->lexer);
        switch (token)
        {
        case Plus:
        {
            return new_scalar_i64(123);
        }

        default:
        {
            err_msg = (str_t)storm_malloc(24);
            snprintf(err_msg, 24, "unexpected token: '%c'", parser->lexer->source[parser->lexer->index]);
            return new_error(ERR_PARSE, err_msg);
        }
        }
    } while (token != Nil);

    return new_scalar_i64(123);
}

extern value_t parse(str_t filename, str_t input)
{
    lexer_t lexer;
    parser_t parser;
    value_t result_t;

    lexer = new_lexer(input);
    parser = new_parser(filename, lexer);

    result_t = parse_program(parser);

    storm_free(parser);
    storm_free(lexer);

    return result_t;
}