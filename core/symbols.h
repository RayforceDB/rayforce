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
#include "mmap.h"
#include "string.h"

#define STRINGS_POOL_SIZE 4096

typedef struct symbols_pool_t
{
    str_p pool_base;
    u64_t pools_count;
    u64_t symbols_count;

} *symbols_pool_p;

/*
Holds memory for strings pool as a node in a linked list
*/
typedef struct pool_node_t
{
    struct pool_node_t *next;
} *pool_node_p;

typedef struct symbol_t
{
    u64_t len;
    i8_t str[];
} *symbol_p;

/*
 *Intern symbols here. Assume symbols are never freed.
 */
typedef struct symbols_t
{
    i64_t next_sym_id;
    obj_p str_po_id;
    obj_p id_to_str;
    pool_node_p pool_node_0;
    pool_node_p pool_node;
    symbol_p symbols_pool;
} *symbols_p;

i64_t intern_symbol(lit_p s, u64_t len);
symbols_p symbols_create(nil_t);
nil_t symbols_free(symbols_p symbols);
str_p strof_sym(i64_t key);
u64_t symbols_count(symbols_p symbols);
u64_t symbols_memsize(symbols_p symbols);

#endif // SYMBOLS_H
