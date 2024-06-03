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
#include "atomic.h"

/*
 * Simplefied version of murmurhash
 */
static inline u64_t str_hash(lit_p str, u64_t len)
{
    u64_t i, k, k1;
    u64_t hash = 0x1234ABCD1234ABCD;
    u64_t c1 = 0x87c37b91114253d5ULL;
    u64_t c2 = 0x4cf5ad432745937fULL;
    const i32_t r1 = 31;
    const i32_t r2 = 27;
    const u64_t m = 5ULL;
    const u64_t n = 0x52dce729ULL;

    // Process each 8-byte block of the key
    for (i = 0; i + 7 < len; i += 8)
    {
        k = (u64_t)str[i] |
            ((u64_t)str[i + 1] << 8) |
            ((u64_t)str[i + 2] << 16) |
            ((u64_t)str[i + 3] << 24) |
            ((u64_t)str[i + 4] << 32) |
            ((u64_t)str[i + 5] << 40) |
            ((u64_t)str[i + 6] << 48) |
            ((u64_t)str[i + 7] << 56);

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
        k1 ^= ((u64_t)str[i + 6]) << 48; // fall through
    case 6:
        k1 ^= ((u64_t)str[i + 5]) << 40; // fall through
    case 5:
        k1 ^= ((u64_t)str[i + 4]) << 32; // fall through
    case 4:
        k1 ^= ((u64_t)str[i + 3]) << 24; // fall through
    case 3:
        k1 ^= ((u64_t)str[i + 2]) << 16; // fall through
    case 2:
        k1 ^= ((u64_t)str[i + 1]) << 8; // fall through
    case 1:
        k1 ^= ((u64_t)str[i]);
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

str_p string_intern(symbols_p symbols, lit_p str, u64_t len)
{
    u64_t rounds = 0, cap;
    str_p curr, node;

    cap = len + 1;
    curr = __atomic_fetch_add(&symbols->string_curr, cap, __ATOMIC_RELAXED);
    node = __atomic_load_n(&symbols->string_node, __ATOMIC_ACQUIRE);

    while ((i64_t)(curr + cap) >= (i64_t)node)
    {
        if ((i64_t)node == NULL_I64)
        {
            backoff_spin(&rounds);
            node = __atomic_load_n(&symbols->string_node, __ATOMIC_ACQUIRE);
            continue;
        }

        // Attempt to commit more memory
        if (__atomic_compare_exchange_n(&symbols->string_node, &node, (str_p)NULL_I64, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        {
            if (mmap_commit(node, STRING_NODE_SIZE) != 0)
            {
                perror("mmap_commit");
                exit(1);
            }

            node += STRING_NODE_SIZE;
            __atomic_store_n(&symbols->string_node, node, __ATOMIC_RELEASE);
        }
        else
        {
            backoff_spin(&rounds);
            node = __atomic_load_n(&symbols->string_node, __ATOMIC_ACQUIRE);
        }
    }

    // Copy the string into the allocated space
    memcpy(curr, str, len);
    curr[len] = '\0';

    return curr;
}

i64_t symbols_intern(lit_p str, u64_t len)
{
    u64_t rounds = 0;
    i64_t index;
    str_p intr;
    symbols_p symbols = runtime_get()->symbols;
    symbol_p new_bucket, current_bucket, b, *syms;

    syms = symbols->syms;
    index = str_hash(str, len) % symbols->size;

load:
    current_bucket = __atomic_load_n(&syms[index], __ATOMIC_ACQUIRE);
    b = current_bucket;

    if ((i64_t)b == NULL_I64)
    {
        backoff_spin(&rounds);
        goto load;
    }

    while (b != NULL)
    {
        if (str_cmp(b->str, b->len, str, len) == 0)
            return (i64_t)b->str;

        b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE);
    }

    if (!__atomic_compare_exchange_n(&syms[index], &current_bucket, (symbol_p)NULL_I64,
                                     1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        backoff_spin(&rounds);
        goto load;
    }

    b = current_bucket;

    while (b != NULL)
    {
        if (str_cmp(b->str, b->len, str, len) == 0)
        {
            __atomic_store_n(&syms[index], current_bucket, __ATOMIC_RELEASE);
            return (i64_t)b->str;
        }

        b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE);
    }

    new_bucket = (symbol_p)heap_alloc(sizeof(struct symbol_t));
    if (new_bucket == NULL)
        return NULL_I64;

    intr = string_intern(symbols, str, len);
    new_bucket->len = len;
    new_bucket->str = intr;
    new_bucket->next = current_bucket;

    __atomic_store_n(&syms[index], new_bucket, __ATOMIC_RELEASE);
    __atomic_fetch_add(&symbols->count, 1, __ATOMIC_RELAXED);

    return (i64_t)intr;
}

symbols_p symbols_create(nil_t)
{
    symbols_p symbols;
    raw_p pooladdr;
    str_p string_pool;

    symbols = (symbols_p)heap_mmap(sizeof(struct symbols_t));

    if (symbols == NULL)
    {
        perror("symbols mmap");
        exit(1);
    }

    // Allocate the string pool as close to the start of the address space as possible
    // #ifdef DEBUG
    pooladdr = NULL;
    // #else
    // pooladdr = (raw_p)(16 * PAGE_SIZE);
    // #endif

    symbols->size = SYMBOLS_HT_SIZE;
    symbols->count = 0;
    symbols->syms = (symbol_p *)heap_mmap(SYMBOLS_HT_SIZE * sizeof(symbol_p));
    string_pool = (str_p)mmap_reserve(pooladdr, STRING_POOL_SIZE);

    if (string_pool == NULL)
    {
        perror("string_pool mmap_reserve");

        // try to reserve memory without specifying the address
        string_pool = (str_p)mmap_reserve(NULL, STRING_POOL_SIZE);

        if (string_pool == NULL)
        {
            perror("string_pool mmap_reserve");
            exit(1);
        }
    }

    symbols->string_pool = string_pool;
    symbols->string_curr = symbols->string_pool;
    symbols->string_node = symbols->string_pool + STRING_NODE_SIZE;

    if (mmap_commit(symbols->string_pool, STRING_NODE_SIZE) == -1)
    {
        perror("string_pool mmap_commit");
        exit(1);
    }

    return symbols;
}

nil_t symbols_destroy(symbols_p symbols)
{
    u64_t i;
    symbol_p b, next;

    // free the symbol pool nodes
    for (i = 0; i < symbols->size; i++)
    {
        b = symbols->syms[i];
        while (b != NULL)
        {
            next = b->next;
            heap_free(b);
            b = next;
        }
    }

    mmap_free(symbols->syms, symbols->size * sizeof(symbol_p));
    mmap_free(symbols->string_pool, STRING_POOL_SIZE);
    heap_unmap(symbols, sizeof(struct symbols_t));
}

str_p str_from_symbol(i64_t key)
{
    return (str_p)key;
}

u64_t symbols_count(symbols_p symbols)
{
    return symbols->count;
}

// TODO
nil_t symbols_rebuild(symbols_p symbols)
{
    unused(symbols);
    // u64_t i, size, new_size;
    // symbol_p bucket, *syms, *new_syms;

    // syms = symbols->syms;
    // size = symbols->size;
    // new_size = size * 2;
    // new_syms = (symbol_p *)heap_mmap(new_size * sizeof(symbol_p));

    // if (new_syms == NULL)
    // {
    //     debug("Memory allocation failed during symbols rebuild.");
    //     return;
    // }

    // // Rehash all elements from the old table to the new table
    // for (i = 0; i < size; ++i)
    // {
    //     bucket = syms[i];
    //     while (bucket != NULL)
    //     {
    //         ht_bk_insert(new_ht, bucket->key, bucket->val);
    //         bucket = bucket->next;
    //     }
    // }

    // // Free the old hash table
    // ht_bk_destroy(*ht);

    // // Update the pointer to point to the new hash table
    // *ht = new_ht;
}