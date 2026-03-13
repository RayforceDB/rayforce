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
#include "str.h"
#include "symbols.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"
#include "runtime.h"
#include "atomic.h"
#include "ops.h"

str_p string_intern(symbols_p symbols, lit_p str, i64_t len) {
    i64_t rounds = 0, cap;
    str_p curr, node;

    assert(len > 0);

    cap = sizeof(u32_t) + len + 1;
    cap = ALIGNUP(cap, sizeof(u32_t));  // Align to 4-byte boundary for proper u32_t alignment
    curr = __atomic_fetch_add(&symbols->string_curr, cap, __ATOMIC_RELAXED);
    node = __atomic_load_n(&symbols->string_node, __ATOMIC_ACQUIRE);

    while ((i64_t)(curr + cap) >= (i64_t)node) {
        if ((i64_t)node == NULL_I64) {
            backoff_spin(&rounds);
            node = __atomic_load_n(&symbols->string_node, __ATOMIC_ACQUIRE);
            continue;
        }

        // Attempt to commit more memory
        if (__atomic_compare_exchange_n(&symbols->string_node, &node, (str_p)NULL_I64, 1, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            if (mmap_commit(node, STRING_NODE_SIZE) != 0) {
                perror("mmap_commit");
                exit(1);
            }

            node += STRING_NODE_SIZE;
            __atomic_store_n(&symbols->string_node, node, __ATOMIC_RELEASE);
        } else {
            backoff_spin(&rounds);
            node = __atomic_load_n(&symbols->string_node, __ATOMIC_ACQUIRE);
        }
    }

    // Copy the string into the allocated space
    *((u32_t *)curr) = len;
    node = curr + sizeof(u32_t);
    memcpy(node, str, len);
    node[len] = '\0';

    return node;
}

i64_t symbols_intern(lit_p str, i64_t len) {
    i64_t l, index, rounds = 0, compact_id;
    str_p intr;
    symbols_p symbols = runtime_get()->symbols;
    symbol_p new_bucket, current_bucket, b, *syms;

    if (len == 0)
        return NULL_I64;

    syms = symbols->syms;
    index = str_hash(str, len) % symbols->size;

load:
    current_bucket = __atomic_load_n(&syms[index], __ATOMIC_ACQUIRE);
    b = current_bucket;

    if ((i64_t)b == NULL_I64) {
        backoff_spin(&rounds);
        goto load;
    }

    while (b != NULL) {
        l = *((u32_t *)(b->str - sizeof(u32_t)));  // Get string length directly
        if (str_cmp(b->str, l, str, len) == 0)
            return b->compact_id;  // Return compact ID instead of pointer

        b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE);
    }

    if (!__atomic_compare_exchange_n(&syms[index], &current_bucket, (symbol_p)NULL_I64, 1, __ATOMIC_ACQUIRE,
                                     __ATOMIC_RELAXED)) {
        backoff_spin(&rounds);
        goto load;
    }

    b = current_bucket;

    while (b != NULL) {
        l = *((u32_t *)(b->str - sizeof(u32_t)));  // Get string length directly
        if (str_cmp(b->str, l, str, len) == 0) {
            __atomic_store_n(&syms[index], current_bucket, __ATOMIC_RELEASE);
            return b->compact_id;  // Return compact ID instead of pointer
        }

        b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE);
    }

    // Allocate new compact ID atomically
    compact_id = __atomic_fetch_add(&symbols->count, 1, __ATOMIC_RELAXED);

    if (compact_id >= (i64_t)SYMBOLS_MAX_COUNT) {
        fprintf(stderr, "Error: exceeded maximum symbol count (%llu)\n", (unsigned long long)SYMBOLS_MAX_COUNT);
        exit(1);
    }

    new_bucket = (symbol_p)heap_alloc(sizeof(struct symbol_t));
    if (new_bucket == NULL)
        return NULL_I64;

    intr = string_intern(symbols, str, len);
    new_bucket->str = intr;
    new_bucket->compact_id = compact_id;
    new_bucket->next = current_bucket;

    // Ensure strings array has committed pages for this compact_id
    {
        raw_p slot_end = (raw_p)&symbols->strings[compact_id + 1];
        raw_p node = __atomic_load_n(&symbols->strings_node, __ATOMIC_ACQUIRE);
        i64_t rounds2 = 0;

        while ((i64_t)node == NULL_I64 || slot_end > node) {
            if ((i64_t)node == NULL_I64) {
                backoff_spin(&rounds2);
                node = __atomic_load_n(&symbols->strings_node, __ATOMIC_ACQUIRE);
                continue;
            }

            if (__atomic_compare_exchange_n(&symbols->strings_node, &node, (raw_p)NULL_I64, 1,
                                             __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                if (mmap_commit(node, SYMBOLS_COMMIT_SIZE) != 0) {
                    perror("symbols strings mmap_commit");
                    exit(1);
                }
                node += SYMBOLS_COMMIT_SIZE;
                __atomic_store_n(&symbols->strings_node, node, __ATOMIC_RELEASE);
            } else {
                backoff_spin(&rounds2);
                node = __atomic_load_n(&symbols->strings_node, __ATOMIC_ACQUIRE);
            }
        }
    }

    // Store string pointer in the lookup array
    symbols->strings[compact_id] = intr;

    __atomic_store_n(&syms[index], new_bucket, __ATOMIC_RELEASE);

    return compact_id;
}

symbols_p symbols_create(nil_t) {
    symbols_p symbols;
    raw_p pooladdr;
    str_p string_pool;

    symbols = (symbols_p)heap_mmap(sizeof(struct symbols_t));

    if (symbols == NULL) {
        perror("symbols mmap");
        exit(1);
    }

    // Allocate the string pool as close to the start of the address space as possible
    pooladdr = (raw_p)(RAY_PAGE_SIZE);
    symbols->size = SYMBOLS_HT_SIZE;
    symbols->count = 0;
    symbols->syms = (symbol_p *)heap_mmap(SYMBOLS_HT_SIZE * sizeof(symbol_p));
    string_pool = (str_p)mmap_reserve(pooladdr, STRING_POOL_SIZE);

    if (string_pool == NULL) {
        perror("string_pool mmap_reserve");
        exit(1);
    }

    symbols->string_pool = string_pool;
    symbols->string_curr = symbols->string_pool;
    symbols->string_node = symbols->string_pool + STRING_NODE_SIZE;

    // Reserve virtual address space for compact_id -> string pointer lookup array
    // Only commit pages incrementally as symbols are created
    symbols->strings = (str_p *)mmap_reserve(NULL, SYMBOLS_MAX_COUNT * sizeof(str_p));
    if (symbols->strings == NULL) {
        perror("symbols->strings mmap_reserve");
        exit(1);
    }

    // Commit initial page
    if (mmap_commit(symbols->strings, SYMBOLS_COMMIT_SIZE) != 0) {
        perror("symbols->strings mmap_commit");
        exit(1);
    }
    symbols->strings_node = (raw_p)symbols->strings + SYMBOLS_COMMIT_SIZE;

    if (mmap_commit(symbols->string_pool, STRING_NODE_SIZE) == -1) {
        perror("string_pool mmap_commit");
        exit(1);
    }

    return symbols;
}

nil_t symbols_destroy(symbols_p symbols) {
    i64_t i;
    symbol_p b, next;

    // free the symbol pool nodes
    for (i = 0; i < symbols->size; i++) {
        b = symbols->syms[i];
        while (b != NULL) {
            next = b->next;
            heap_free(b);
            b = next;
        }
    }

    mmap_free(symbols->syms, symbols->size * sizeof(symbol_p));
    mmap_free((raw_p)symbols->strings, SYMBOLS_MAX_COUNT * sizeof(str_p));
    mmap_free(symbols->string_pool, STRING_POOL_SIZE);
    heap_unmap(symbols, sizeof(struct symbols_t));
}

str_p str_from_symbol(i64_t compact_id) {
    if (compact_id == NULL_I64)
        return (str_p)"";
    return runtime_get()->symbols->strings[compact_id];
}

i64_t symbol_strlen(i64_t compact_id) {
    if (compact_id == NULL_I64)
        return 0;
    str_p s = runtime_get()->symbols->strings[compact_id];
    return *((u32_t *)(s - sizeof(u32_t)));
}

i64_t symbols_count(symbols_p symbols) { return symbols->count; }

// TODO
nil_t symbols_rebuild(symbols_p symbols) {
    UNUSED(symbols);
    // i64_t i, size, new_size;
    // symbol_p bucket, *syms, *new_syms;

    // syms = symbols->syms;
    // size = symbols->size;
    // new_size = size * 2;
    // new_syms = (symbol_p *)heap_mmap(new_size * sizeof(symbol_p));

    // if (new_syms == NULL)
    // {
    //     DEBUG_PRINT("Memory allocation failed during symbols rebuild.");
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