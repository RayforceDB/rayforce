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

/*
Holds memory for strings pool as a node in a linked list
*/
typedef struct pool_node_t
{
    struct pool_node_t *next;
} pool_node_t;

/*
Intern symbols here. Assume symbols are never freed.
*/
typedef struct symbols_t
{
    hash_table_t *str_to_id;
    hash_table_t *id_to_str;
    pool_node_t *pool_node_0;
    pool_node_t *pool_node;
    str_t strings_pool;
} symbols_t;

i64_t symbols_intern(rf_object_t *string);
str_t symbols_get(i64_t key);

symbols_t *symbols_new();
null_t symbols_free(symbols_t *symbols);

#endif
