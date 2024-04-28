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

#define SYMBOLS_POOL_SIZE 4096

/*
 * Simplefied version of murmurhash
 */
u64_t str_hash(lit_p key, u64_t len)
{
    u64_t i, k, k1;
    u64_t hash = 0x1234ABCD1234ABCD;
    u64_t c1 = 0x87c37b91114253d5ULL;
    u64_t c2 = 0x4cf5ad432745937fULL;
    const int r1 = 31;
    const int r2 = 27;
    const u64_t m = 5ULL;
    const u64_t n = 0x52dce729ULL;

    // Process each 8-byte block of the key
    for (i = 0; i + 7 < len; i += 8)
    {
        k = (u64_t)key[i] |
            ((u64_t)key[i + 1] << 8) |
            ((u64_t)key[i + 2] << 16) |
            ((u64_t)key[i + 3] << 24) |
            ((u64_t)key[i + 4] << 32) |
            ((u64_t)key[i + 5] << 40) |
            ((u64_t)key[i + 6] << 48) |
            ((u64_t)key[i + 7] << 56);

        k *= c1;
        k = (k << r1) | (k >> (64 - r1));
        k *= c2;

        hash ^= k;
        hash = ((hash << r2) | (hash >> (64 - r2))) * m + n;
    }

    // Process the tail of the data
    k1 = 0;
    switch (len & 7)
    {
    case 7:
        k1 ^= ((u64_t)key[i + 6]) << 48; // fall through
    case 6:
        k1 ^= ((u64_t)key[i + 5]) << 40; // fall through
    case 5:
        k1 ^= ((u64_t)key[i + 4]) << 32; // fall through
    case 4:
        k1 ^= ((u64_t)key[i + 3]) << 24; // fall through
    case 3:
        k1 ^= ((u64_t)key[i + 2]) << 16; // fall through
    case 2:
        k1 ^= ((u64_t)key[i + 1]) << 8; // fall through
    case 1:
        k1 ^= ((u64_t)key[i]);
        k1 *= c1;
        k1 = (k1 << r1) | (k1 >> (64 - r1));
        k1 *= c2;
        hash ^= k1;
    }

    // Finalize the hash
    hash ^= len;
    hash ^= (hash >> 33);
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= (hash >> 33);
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= (hash >> 33);

    return hash;
}

u64_t string_hash(i64_t key, raw_p seed)
{
    unused(seed);
    symbol_p sym = (symbol_p)key;

    return str_hash(sym->str, sym->len);
}

i64_t str_cmp(lit_p a, u64_t aln, lit_p b, u64_t bln)
{
    if (aln != bln)
        return 1;

    return strncmp(a, b, aln);
}

i64_t symbols_next(obj_p *obj, lit_p str, u64_t len)
{
    u64_t i, size;
    i64_t *keys;
    symbol_p sym;

    size = as_list(*obj)[0]->len;
    keys = as_i64(as_list(*obj)[0]);

next:
    for (i = str_hash(str, len) & (size - 1); i < size; i++)
    {
        sym = (symbol_p)keys[i];
        if (keys[i] == NULL_I64 || str_cmp(sym->str, sym->len, str, len) == 0)
            return i;
    }

    rehash(obj, &string_hash, NULL);

    size = as_list(*obj)[0]->len;
    keys = as_i64(as_list(*obj)[0]);

    goto next;
}

/*
 * Allocate a new pool node for the strings pool.
 */
pool_node_p pool_node_new(nil_t)
{
    return (pool_node_p)heap_mmap(STRINGS_POOL_SIZE);
}

nil_t pool_node_free(pool_node_p node)
{
    heap_unmap(node, STRINGS_POOL_SIZE);
}

symbol_p str_intern(symbols_p symbols, lit_p str, u64_t len)
{
    symbol_p sym = symbols->symbols_pool;
    u64_t size = alignup(sizeof(struct symbol_t) + len + 1, sizeof(struct symbol_t));
    pool_node_p node;

    // Allocate new pool node
    if ((i64_t)symbols->symbols_pool + size - (i64_t)symbols->pool_node >= STRINGS_POOL_SIZE)
    {
        node = pool_node_new();
        symbols->pool_node->next = node;
        symbols->pool_node = node;
        symbols->symbols_pool = (symbol_p)(node + sizeof(pool_node_p)); // Skip the node size of next ptr
        sym = symbols->symbols_pool;
    }

    sym->len = len;
    memcpy(sym->str, str, len);
    sym->str[len] = '\0';
    symbols->symbols_pool += size;

    return sym;
}

symbols_p symbols_new(nil_t)
{
    symbols_p symbols = (symbols_p)heap_mmap(sizeof(struct symbols_t));
    pool_node_p node = pool_node_new();

    pthread_mutex_init(&symbols->lock, NULL);
    symbols->pool_node_0 = node;
    symbols->pool_node = node;
    symbols->symbols_pool = (symbol_p)(node + sizeof(pool_node_p)); // Skip the node size of next ptr
    symbols->str_po_id = ht_tab(SYMBOLS_POOL_SIZE, TYPE_I64);
    symbols->id_to_str = ht_tab(SYMBOLS_POOL_SIZE, TYPE_I64);
    symbols->next_sym_id = 0;

    return symbols;
}

nil_t symbols_free(symbols_p symbols)
{
    pool_node_p node = symbols->pool_node_0;

    while (node)
    {
        pool_node_p next = node->next;
        pool_node_free(node);
        node = next;
    }

    drop_obj(symbols->str_po_id);
    drop_obj(symbols->id_to_str);
}

i64_t intern_symbol(lit_p s, u64_t len)
{
    symbol_p sym;
    symbols_p symbols = runtime_get()->symbols;
    i64_t idx;

    // insert new symbol
    // pthread_mutex_lock(&symbols->lock);
    idx = symbols_next(&symbols->str_po_id, s, len);
    if (as_i64(as_list(symbols->str_po_id)[0])[idx] == NULL_I64)
    {
        sym = str_intern(symbols, s, len);
        as_i64(as_list(symbols->str_po_id)[0])[idx] = (i64_t)sym;
        as_i64(as_list(symbols->str_po_id)[1])[idx] = symbols->next_sym_id;

        // insert id into id_to_str
        idx = ht_tab_next(&symbols->id_to_str, symbols->next_sym_id);
        debug_assert(as_i64(as_list(symbols->id_to_str)[0])[idx] == NULL_I64, "Symbol id: '%lld' already exists",
                     symbols->next_sym_id);
        as_i64(as_list(symbols->id_to_str)[0])[idx] = symbols->next_sym_id;
        as_i64(as_list(symbols->id_to_str)[1])[idx] = (i64_t)sym;

        // pthread_mutex_unlock(&symbols->lock);

        return symbols->next_sym_id++;
    }
    // symbol is already interned
    else
    {
        // pthread_mutex_unlock(&symbols->lock);
        return as_i64(as_list(symbols->str_po_id)[1])[idx];
    }
}

str_p strof_sym(i64_t key)
{
    symbols_p symbols = runtime_get()->symbols;
    i64_t idx = ht_tab_next(&symbols->id_to_str, key);
    symbol_p sym;

    if (as_i64(as_list(symbols->id_to_str)[0])[idx] == NULL_I64)
        return (str_p) "";

    sym = (symbol_p)as_i64(as_list(symbols->id_to_str)[1])[idx];

    return sym->str;
}

u64_t symbols_count(symbols_p symbols)
{
    return symbols->next_sym_id;
}

u64_t symbols_memsize(symbols_p symbols)
{
    pool_node_p node = symbols->pool_node_0;
    u64_t size = 0;

    // calculate all nodes
    while (node)
    {
        size += STRINGS_POOL_SIZE;
        node = node->next;
    }

    return size;
}
