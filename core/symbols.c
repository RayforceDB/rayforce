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
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "symbols.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"
#include "runtime.h"

#define SYMBOLS_POOL_SIZE 4096 * 8

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

i32_t i64_cmp(i64_t a, i64_t b)
{
    return a != b;
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

/*
 * This callback will be called on new buckets being added to a hash table.
 * In case of symbols this avoids having to copy the string every time.
 * Stores string in the strings pool and returns the pointer to the string.
 */
i64_t str_dup(i64_t key, i64_t val, nil_t *seed, i64_t *tkey, i64_t *tval)
{
    symbols_t *symbols = (symbols_t *)seed;

    str_slice_t *string = (str_slice_t *)key;
    str_t str = string->str;
    i64_t len = string->len;

    // Allocate new pool node
    if ((i64_t)symbols->strings_pool + len - (i64_t)symbols->pool_node + 1 >= STRINGS_POOL_SIZE)
    {
        pool_node_t *node = pool_node_new();
        symbols->pool_node->next = node;
        symbols->pool_node = node;
        symbols->strings_pool = (str_t)(node + sizeof(pool_node_t *)); // Skip the node size of next ptr
    }
    strncpy(symbols->strings_pool, str, len);
    *tkey = (i64_t)symbols->strings_pool;
    *tval = val;
    symbols->strings_pool += len + 1; // +1 for null terminator (buffer is zeroed)
    return *tkey;
}

symbols_t *symbols_new()
{
    symbols_t *symbols = (symbols_t *)mmap_malloc(sizeof(struct symbols_t));

    pool_node_t *node = pool_node_new();
    symbols->pool_node_0 = node;
    symbols->pool_node = node;
    symbols->strings_pool = (str_t)(node + sizeof(pool_node_t *)); // Skip the node size of next ptr

    symbols->str_to_id = ht_new(SYMBOLS_POOL_SIZE, &string_hash, &string_str_cmp);
    symbols->id_to_str = ht_new(SYMBOLS_POOL_SIZE, &rfi_kmh_hash, &i64_cmp);
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

    ht_free(symbols->str_to_id);
    ht_free(symbols->id_to_str);
}

i64_t intern_symbol(str_t s, i64_t len)
{
    symbols_t *symbols = runtime_get()->symbols;
    str_slice_t str_slice = {s, len};
    str_t p = symbols->strings_pool;
    i64_t id = symbols->next_sym_id,
          id_or_str = ht_insert_with(symbols->str_to_id, (i64_t)&str_slice, id, symbols, &str_dup);

    // symbol is already interned (strings pool pointer is not moved)
    if (p == symbols->strings_pool)
        return id_or_str;

    ht_insert(symbols->id_to_str, id, id_or_str);
    symbols->next_sym_id++;
    return id;
}

i64_t intern_keyword(str_t s, i64_t len)
{
    symbols_t *symbols = runtime_get()->symbols;
    str_slice_t str_slice = {s, len};
    str_t p = symbols->strings_pool;
    i64_t id = symbols->next_kw_id,
          id_or_str = ht_insert_with(symbols->str_to_id, (i64_t)&str_slice, id, symbols, &str_dup);

    // symbol is already interned (strings pool pointer is not moved)
    if (p == symbols->strings_pool)
        return id_or_str;

    ht_insert(symbols->id_to_str, id, id_or_str);
    symbols->next_kw_id--;
    return id;
}

str_t symbols_get(i64_t key)
{
    symbols_t *symbols = runtime_get()->symbols;
    return (str_t)ht_get(symbols->id_to_str, key);
}
