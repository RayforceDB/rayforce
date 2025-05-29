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

#ifndef SYMBOLS_H
#define SYMBOLS_H

#include "rayforce.h"
#include "hash.h"

#define SYMBOLS_HT_SIZE RAY_PAGE_SIZE * 1024
#define STRING_NODE_SIZE RAY_PAGE_SIZE
#define STRING_POOL_SIZE (RAY_PAGE_SIZE * 1024ull * 1024ull)
#define SYMBOL_STRLEN(x) ((x == NULL_I64) ? 0 : *((u32_t *)(x - sizeof(u32_t))))

typedef struct symbol_t {
    lit_p str;
    struct symbol_t *next;
} *symbol_p;

typedef struct symbols_t {
    i64_t size;
    i64_t count;
    symbol_p *syms;
    str_p string_pool;  // string pool
    str_p string_node;  // string pool current node
    str_p string_curr;  // string pool cursor
} *symbols_p;

i64_t symbols_intern(lit_p s, i64_t len);
symbols_p symbols_create(nil_t);
nil_t symbols_destroy(symbols_p symbols);
i64_t symbols_count(symbols_p symbols);
str_p str_from_symbol(i64_t key);
nil_t symbols_rebuild(symbols_p symbols);

#endif  // SYMBOLS_H
