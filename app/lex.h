#ifndef LEX_H
#define LEX_H

#include "../core/storm.h"

typedef enum token_t
{
    Invalid = -1, // Invalid token
    Nil,          // \0
    Lparen,       // (
    Rparen,       // )
    Lbrace,       // {
    Rbrace,       // }
    Lbracket,     // [
    Rbracket,     // ]
    Comma,        // ,
    Dot,          // .
    Minus,        // -
    Plus,         // +
    Semicolon,    // ;
    Slash,        // /
    Star,         // *
    Bang,         // !
    BangEqual,    // !=
} token_t;

typedef struct lexer_t
{
    str_t source;
    i64_t index;
    i64_t line;
    i64_t column;
} __attribute__((aligned(16))) * lexer_t;

lexer_t new_lexer(str_t source);
token_t next_token(lexer_t lexer);

#endif
