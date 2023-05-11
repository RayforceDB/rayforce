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
#include "alloc.h"
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
i64_t string_hash(null_t *val)
{
    str_slice_t *string = (str_slice_t *)val;
    i32_t hash = 5381, len = string->len, i;
    str_t str = string->str;

    for (i = 0; i < len; i++)
        hash += (hash << 5) + str[i];

    return hash;
}

i64_t i64_hash(null_t *val)
{
    return (i64_t)val;
}

i32_t i64_cmp(null_t *a, null_t *b)
{
    return !((i64_t)a == (i64_t)b);
}

/*
 * Compares str_slice_t with null terminated string.
 * Returns 0 if equal, 1 if not equal.
 * Should not be used elsewere but symbols
 * a: raw C string already stored in a hash
 * b: str_slice_t string to be inserted
 */
i32_t string_str_cmp(null_t *a, null_t *b)
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
    pool_node_t *node = (pool_node_t *)mmap(NULL, STRINGS_POOL_SIZE,
                                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(node, 0, STRINGS_POOL_SIZE);

    return node;
}

null_t pool_node_free(pool_node_t *node)
{
    munmap(node, STRINGS_POOL_SIZE);
}

/*
 * This callback will be called on new buckets being added to a hash table.
 * In case of symbols this avoids having to copy the string every time.
 * Stores string in the strings pool and returns the pointer to the string.
 */
null_t *str_dup(null_t *key, null_t *val, bucket_t *bucket)
{
    symbols_t *symbols = runtime_get()->symbols;

    str_slice_t *string = (str_slice_t *)key;
    str_t str = string->str;
    i64_t len = string->len;

    // Allocate new pool node
    if ((i64_t)symbols->strings_pool + len - (i64_t)symbols->pool_node > STRINGS_POOL_SIZE)
    {
        pool_node_t *node = pool_node_new();
        symbols->pool_node->next = node;
        symbols->pool_node = node;
        symbols->strings_pool = (str_t)(node + sizeof(pool_node_t *)); // Skip the node size of next ptr
    }

    strncpy(symbols->strings_pool, str, len);
    bucket->key = symbols->strings_pool;
    bucket->val = val;
    symbols->strings_pool += len + 1; // +1 for null terminator (buffer is zeroed)
    return bucket->key;
}

symbols_t *symbols_new()
{
    symbols_t *symbols = (symbols_t *)mmap(NULL, sizeof(symbols_t),
                                           PROT_READ | PROT_WRITE,
                                           MAP_ANONYMOUS | MAP_PRIVATE,
                                           -1, 0);

    pool_node_t *node = pool_node_new();
    symbols->pool_node_0 = node;
    symbols->pool_node = node;
    symbols->strings_pool = (str_t)(node + sizeof(pool_node_t *)); // Skip the node size of next ptr

    symbols->str_to_id = ht_new(SYMBOLS_POOL_SIZE, &string_hash, &string_str_cmp);
    symbols->id_to_str = ht_new(SYMBOLS_POOL_SIZE, &i64_hash, &i64_cmp);

    return symbols;
}

null_t symbols_free(symbols_t *symbols)
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
    munmap(symbols->str_to_id, sizeof(struct hash_table_t));
    munmap(symbols->id_to_str, sizeof(struct hash_table_t));
}

i64_t symbols_intern(str_t s, i64_t len)
{
    symbols_t *symbols = runtime_get()->symbols;
    i64_t id = symbols->str_to_id->size;
    str_slice_t str_slice = {s, len};
    i64_t id_or_str = (i64_t)ht_insert_with(symbols->str_to_id, &str_slice, (null_t *)id, &str_dup);
    if (symbols->str_to_id->size == id)
        return id_or_str;

    ht_insert(symbols->id_to_str, (null_t *)id, (str_t)id_or_str);
    return id;
}

str_t symbols_get(i64_t key)
{
    symbols_t *symbols = runtime_get()->symbols;
    return (str_t)ht_get(symbols->id_to_str, (null_t *)key);
}
