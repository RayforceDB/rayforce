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

#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "string.h"
#include "symbols.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"
#include "runtime.h"
#include "ops.h"

#define SYMBOLS_POOL_SIZE 128

typedef struct str_slice_t
{
    str_t str;
    i64_t len;
} str_slice_t;

/*
 * Improved djb2 (contains length)
 * by Dan Bernstein
 * http://www.cse.yorku.ca/~oz/hash.html
 */
u64_t string_hash(i64_t val)
{
    str_slice_t *string = (str_slice_t *)val;
    u64_t hash = 5381, len = string->len, i;
    str_t str = string->str;

    for (i = 0; i < len; i++)
        hash += (hash << 5) + str[i];

    return hash;
}

/*
 * Compares str_slice_t with null terminated string.
 * Returns 0 if equal, 1 if not equal.
 * Should not be used elsewere but symbols
 * a: raw C string already stored in a hash
 * b: str_slice_t string to be inserted
 */
i32_t string_str_cmp(i64_t a, i64_t b)
{
    str_t str_a = (str_t)a;
    str_slice_t *str_b = (str_slice_t *)b;

    i64_t len_a = strlen(str_a);

    if (str_b->len != len_a)
        return 1;

    return strncmp(str_b->str, str_a, len_a);
}

/*
 * Allocate a new pool node for the strings pool.
 */
pool_node_t *pool_node_new()
{
    pool_node_t *node = (pool_node_t *)mmap_malloc(STRINGS_POOL_SIZE);
    memset(node, 0, STRINGS_POOL_SIZE);

    return node;
}

nil_t pool_node_free(pool_node_t *node)
{
    mmap_free(node, STRINGS_POOL_SIZE);
}

str_t str_intern(symbols_t *symbols, str_t str, i64_t len)
{
    str_t p = symbols->strings_pool;

    // Allocate new pool node
    if ((i64_t)symbols->strings_pool + len - (i64_t)symbols->pool_node + 1 >= STRINGS_POOL_SIZE)
    {
        pool_node_t *node = pool_node_new();
        symbols->pool_node->next = node;
        symbols->pool_node = node;
        symbols->strings_pool = (str_t)(node + sizeof(pool_node_t *)); // Skip the node size of next ptr
        p = symbols->strings_pool;
    }

    strncpy(symbols->strings_pool, str, len);
    symbols->strings_pool += len + 1; // +1 for null terminator (buffer is zeroed)
    return p;
}

symbols_t *symbols_new()
{
    symbols_t *symbols = (symbols_t *)mmap_malloc(sizeof(struct symbols_t));

    pool_node_t *node = pool_node_new();
    symbols->pool_node_0 = node;
    symbols->pool_node = node;
    symbols->strings_pool = (str_t)(node + sizeof(pool_node_t *)); // Skip the node size of next ptr

    symbols->str_to_id = ht_tab(SYMBOLS_POOL_SIZE, TYPE_I64);
    symbols->id_to_str = ht_tab(SYMBOLS_POOL_SIZE, TYPE_I64);
    symbols->next_sym_id = 0;
    symbols->next_kw_id = -1;

    return symbols;
}

nil_t symbols_free(symbols_t *symbols)
{
    pool_node_t *node = symbols->pool_node_0;

    while (node)
    {
        pool_node_t *next = node->next;
        pool_node_free(node);
        node = next;
    }

    drop(symbols->str_to_id);
    drop(symbols->id_to_str);
}

i64_t intern_symbol(str_t s, i64_t len)
{
    str_t p;
    symbols_t *symbols = runtime_get()->symbols;
    str_slice_t str_slice = {s, len};
    i64_t idx = ht_tab_next_with(&symbols->str_to_id, (i64_t)&str_slice,
                                 &string_hash, &string_str_cmp);

    // insert new symbol
    if (as_i64(as_list(symbols->str_to_id)[0])[idx] == NULL_I64)
    {
        p = str_intern(symbols, s, len);
        as_i64(as_list(symbols->str_to_id)[0])[idx] = (i64_t)p;
        as_i64(as_list(symbols->str_to_id)[1])[idx] = symbols->next_sym_id;

        // insert id into id_to_str
        idx = ht_tab_next(&symbols->id_to_str, symbols->next_sym_id);
        as_i64(as_list(symbols->id_to_str)[0])[idx] = symbols->next_sym_id;
        as_i64(as_list(symbols->id_to_str)[1])[idx] = (i64_t)p;

        return symbols->next_sym_id++;
    }
    // symbol is already interned
    else
        return as_i64(as_list(symbols->str_to_id)[1])[idx];
}

i64_t intern_keyword(str_t s, i64_t len)
{
    str_t p;
    symbols_t *symbols = runtime_get()->symbols;
    str_slice_t str_slice = {s, len};
    i64_t idx = ht_tab_next_with(&symbols->str_to_id, (i64_t)&str_slice,
                                 &string_hash, &string_str_cmp);

    // insert new symbol
    if (as_i64(as_list(symbols->str_to_id)[0])[idx] == NULL_I64)
    {
        p = str_intern(symbols, s, len);
        as_i64(as_list(symbols->str_to_id)[0])[idx] = (i64_t)p;
        as_i64(as_list(symbols->str_to_id)[1])[idx] = symbols->next_kw_id;

        // insert id into id_to_str
        idx = ht_tab_next(&symbols->id_to_str, symbols->next_kw_id);
        as_i64(as_list(symbols->id_to_str)[0])[idx] = symbols->next_kw_id;
        as_i64(as_list(symbols->id_to_str)[1])[idx] = (i64_t)p;

        return symbols->next_kw_id--;
    }
    // symbol is already interned
    else
        return as_i64(as_list(symbols->str_to_id)[1])[idx];
}

str_t symtostr(i64_t key)
{
    symbols_t *symbols = runtime_get()->symbols;
    i64_t idx = ht_tab_next(&symbols->id_to_str, key);
    if (as_i64(as_list(symbols->id_to_str)[0])[idx] == NULL_I64)
        return "";

    return (str_t)as_i64(as_list(symbols->id_to_str)[1])[idx];
}
