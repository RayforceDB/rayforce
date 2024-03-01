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

#ifndef PARSE_H
#define PARSE_H

#include "rayforce.h"
#include "nfo.h"

/*
 * Parser structure
 */
typedef struct parser_t
{
    obj_p nfo;            // debug info for current parser
    i64_t count;          // counter for spans without objects
    str_p input;          // input string
    str_p current;        // current character
    i64_t line;           // current line
    i64_t column;         // current column
    b8_t replace_symbols; // replace symbols if they are internal functions
} __attribute__((aligned(16))) parser_t;

obj_p parser_advance(parser_t *parser);
nil_t parser_free(parser_t *parser);
obj_p parse_do(parser_t *parser);
obj_p parse(str_p input, obj_p nfo);

#endif // PARSE_H
