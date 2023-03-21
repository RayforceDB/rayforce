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

#include "../core/rayforce.h"

/*
 * Points to a actual error position in a source code
 */
typedef struct span_t
{
    u16_t start_line;
    u16_t end_line;
    u16_t start_column;
    u16_t end_column;
} span_t;

/*
 * Parser structure
 */
typedef struct parser_t
{
    str_t filename; // filename
    str_t input;    // input string
    str_t current;  // current character
    i64_t line;     // current line
    i64_t column;   // current column
} __attribute__((aligned(16))) parser_t;

rf_object_t advance(parser_t *parser);

extern rf_object_t parse(str_t filename, str_t input);

#endif
