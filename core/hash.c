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
#include "hash.h"
#include "rayforce.h"
#include "heap.h"
#include "util.h"
#include "eval.h"
#include "error.h"
#include "atomic.h"
#include "pool.h"

i64_t optimal_hash_table_size(i64_t len, f64_t load_factor) {
    i64_t size = (i64_t)CEILF64(len / load_factor);
    return ops_next_prime(size);
}

obj_p ht_oa_create(i64_t size, i8_t vals) {
    i64_t i, adjusted_size;
    obj_p k, v;

    adjusted_size = optimal_hash_table_size(size, 0.75);
    k = vector(TYPE_I64, adjusted_size);

    if (vals >= 0)
        v = vector(vals, adjusted_size);
    else
        v = NULL_OBJ;

    for (i = 0; i < adjusted_size; i++)
        AS_I64(k)[i] = NULL_I64;

    return dict(k, v);
}

nil_t ht_oa_rehash(obj_p *obj, hash_f hash, raw_p seed) {
    i64_t i, j, idx, size, key, start, new_size;
    i8_t type;
    i64_t *orig_keys, *new_keys, *orig_vals = NULL, *new_vals = NULL;
    obj_p new_obj;

    size = AS_LIST(*obj)[0]->len;
    orig_keys = AS_I64(AS_LIST(*obj)[0]);

    type = is_null(AS_LIST(*obj)[1]) ? -1 : AS_LIST(*obj)[1]->type;

    if (type > -1)
        orig_vals = AS_I64(AS_LIST(*obj)[1]);

    new_obj = ht_oa_create(size * 2, type);

    new_size = AS_LIST(new_obj)[0]->len;
    new_keys = AS_I64(AS_LIST(new_obj)[0]);

    if (type > -1)
        new_vals = AS_I64(AS_LIST(new_obj)[1]);

    for (i = 0; i < size; i++) {
        if (orig_keys[i] != NULL_I64) {
            key = orig_keys[i];

            // Recalculate the index for the new table
            start = hash ? hash(key, seed) % new_size : (i64_t)key % new_size;
            idx = start;

            // NOTE: this won't fail because the new table is twice the size of the old one
            for (j = start; j < new_size + start; j++)  // Linear probing with wrap-around
            {
                idx = j % new_size;

                if (new_keys[idx] == NULL_I64)
                    break;
            }

            new_keys[idx] = key;

            if (type > -1)
                new_vals[idx] = orig_vals[i];
        }
    }

    drop_obj(*obj);
    *obj = new_obj;
}

i64_t ht_oa_tab_next(obj_p *obj, i64_t key) {
    i64_t i, idx, size, start;
    i64_t *keys;

    for (;;) {
        size = AS_LIST(*obj)[0]->len;
        keys = AS_I64(AS_LIST(*obj)[0]);

        start = (i64_t)key % size;

        for (i = start; i < size + start; i++) {
            idx = i % size;

            if ((keys[idx] == NULL_I64) || (keys[idx] == key))
                return idx;
        }

        ht_oa_rehash(obj, NULL, NULL);
    }
}

i64_t ht_oa_tab_next_with(obj_p *obj, i64_t key, hash_f hash, cmp_f cmp, raw_p seed) {
    i64_t i, idx, size, start;
    i64_t *keys;

    for (;;) {
        size = AS_LIST(*obj)[0]->len;
        keys = AS_I64(AS_LIST(*obj)[0]);

        start = hash(key, seed) % size;

        for (i = start; i < size + start; i++) {
            idx = i % size;

            if (keys[idx] == NULL_I64 || cmp(keys[idx], key, seed) == 0)
                return idx;
        }

        ht_oa_rehash(obj, hash, seed);
    }
}

i64_t ht_oa_tab_insert(obj_p *obj, i64_t key, i64_t val) {
    i64_t i, idx, size, start;
    i64_t *keys, *vals;

    for (;;) {
        size = AS_LIST(*obj)[0]->len;
        keys = AS_I64(AS_LIST(*obj)[0]);
        vals = AS_I64(AS_LIST(*obj)[1]);

        start = (i64_t)key % size;

        for (i = start; i < size + start; i++) {
            idx = i % size;

            if (keys[idx] == NULL_I64) {
                keys[idx] = key;
                vals[idx] = val;
                return val;
            }

            if (keys[idx] == key)
                return vals[i];
        }

        ht_oa_rehash(obj, NULL, NULL);
    }
}

i64_t ht_oa_tab_insert_with(obj_p *obj, i64_t key, i64_t val, hash_f hash, cmp_f cmp, raw_p seed) {
    i64_t i, idx, size, start;
    i64_t *keys, *vals;

    for (;;) {
        size = AS_LIST(*obj)[0]->len;
        keys = AS_I64(AS_LIST(*obj)[0]);
        vals = AS_I64(AS_LIST(*obj)[1]);

        start = hash(key, seed) % size;

        // Linear probing with wrap-around
        for (i = start; i < size + start; i++) {
            idx = i % size;  // Wrap around to the start of the table

            if (keys[idx] == NULL_I64) {
                keys[idx] = key;
                vals[idx] = val;
                return val;
            }

            if (cmp(keys[idx], key, seed) == 0)
                return vals[idx];
        }

        ht_oa_rehash(obj, hash, seed);
    }
}

i64_t ht_oa_tab_get(obj_p obj, i64_t key) {
    i64_t i, idx, size, start;
    i64_t *keys;

    size = AS_LIST(obj)[0]->len;
    keys = AS_I64(AS_LIST(obj)[0]);

    start = (i64_t)key % size;

    for (i = start; i < size + start; i++) {
        idx = i % size;

        if (keys[idx] == NULL_I64)
            return NULL_I64;
        else if (keys[idx] == key)
            return idx;
    }

    return NULL_I64;
}

i64_t ht_oa_tab_get_with(obj_p obj, i64_t key, hash_f hash, cmp_f cmp, raw_p seed) {
    i64_t i, idx, size, start;
    i64_t *keys;

    size = AS_LIST(obj)[0]->len;
    keys = AS_I64(AS_LIST(obj)[0]);

    start = hash(key, seed) % size;

    for (i = start; i < size + start; i++) {
        idx = i % size;

        if (keys[idx] == NULL_I64)
            return NULL_I64;
        else if (cmp(keys[idx], key, seed) == 0)
            return idx;
    }

    return NULL_I64;
}

i64_t hash_index_obj(obj_p obj) {
    i64_t hash, len, i;

    switch (obj->type) {
        case -TYPE_I16:
            return (i64_t)obj->i16;
        case -TYPE_I32:
        case -TYPE_DATE:
        case -TYPE_TIME:
            return (i64_t)obj->i32;
        case -TYPE_I64:
        case -TYPE_SYMBOL:
        case -TYPE_TIMESTAMP:
            return (i64_t)obj->i64;
        case -TYPE_F64:
            return (i64_t)obj->f64;
        case -TYPE_GUID:
            return hash_index_u64(*(i64_t *)AS_GUID(obj), *((i64_t *)AS_GUID(obj) + 1));
        case TYPE_C8:
            return str_hash(AS_C8(obj), obj->len);
        case TYPE_I16:
            len = obj->len;
            for (i = 0, hash = 0xcbf29ce484222325ull; i < len; i++)
                hash = hash_index_u64((i64_t)AS_I16(obj)[i], hash);
            return hash;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            len = obj->len;
            for (i = 0, hash = 0xcbf29ce484222325ull; i < len; i++)
                hash = hash_index_u64((i64_t)AS_I32(obj)[i], hash);
            return hash;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            len = obj->len;
            for (i = 0, hash = 0xcbf29ce484222325ull; i < len; i++)
                hash = hash_index_u64((i64_t)AS_I64(obj)[i], hash);
            return hash;
        case TYPE_F64:
            len = obj->len;
            for (i = 0, hash = 0xcbf29ce484222325ull; i < len; i++)
                hash = hash_index_u64((i64_t)AS_F64(obj)[i], hash);
            return hash;
        default:
            PANIC("hash: unsupported type: %d", obj->type);
    }
}

ht_bk_p ht_bk_create(i64_t size) {
    i64_t i;
    ht_bk_p ht;

    ht = (ht_bk_p)heap_alloc(sizeof(struct ht_bk_t) + size * sizeof(bucket_p));
    if (ht == NULL)
        return NULL;

    ht->size = size;
    ht->count = 0;
    for (i = 0; i < size; i++)
        ht->table[i] = NULL;

    return ht;
}

nil_t ht_bk_destroy(ht_bk_p ht) {
    i64_t i;
    bucket_p current, next;

    // free buckets
    for (i = 0; i < ht->size; i++) {
        current = ht->table[i];
        while (current != NULL) {
            next = current->next;
            heap_free(current);
            current = next;
        }
    }

    heap_free(ht);
}

nil_t ht_bk_rehash(ht_bk_p *ht, i64_t new_size) {
    i64_t i;
    bucket_p bucket;
    ht_bk_p new_ht = ht_bk_create(new_size);

    if (new_ht == NULL)
        PANIC("Memory allocation failed during rehash.");

    // Rehash all elements from the old table to the new table
    for (i = 0; i < (*ht)->size; ++i) {
        bucket = (*ht)->table[i];
        while (bucket != NULL) {
            ht_bk_insert(new_ht, bucket->key, bucket->val);
            bucket = bucket->next;
        }
    }

    // Free the old hash table
    ht_bk_destroy(*ht);

    // Update the pointer to point to the new hash table
    *ht = new_ht;
}

i64_t ht_bk_insert(ht_bk_p ht, i64_t key, i64_t val) {
    i64_t index;
    bucket_p bucket;

    index = key % ht->size;

    bucket = ht->table[index];

    if (bucket == NULL) {
        bucket = (bucket_p)heap_alloc(sizeof(struct bucket_t));
        if (bucket == NULL)
            return NULL_I64;

        bucket->key = key;
        bucket->val = val;
        bucket->next = NULL;
        ht->table[index] = bucket;
        ht->count++;

        return val;
    }

    while (bucket != NULL) {
        if (bucket->key == key)
            return bucket->val;

        bucket = bucket->next;
    }

    // Key does not exist, insert a new bucket
    bucket = (bucket_p)heap_alloc(sizeof(struct bucket_t));
    if (bucket == NULL)
        return NULL_I64;

    bucket->key = key;
    bucket->val = val;
    bucket->next = ht->table[index];
    ht->table[index] = bucket;
    ht->count++;

    return val;
}

i64_t ht_bk_insert_with(ht_bk_p ht, i64_t key, i64_t val, hash_f hash, cmp_f cmp, raw_p seed) {
    i64_t index;
    bucket_p bucket;

    index = hash(key, seed) % ht->size;

    bucket = ht->table[index];

    if (bucket == NULL) {
        bucket = (bucket_p)heap_alloc(sizeof(struct bucket_t));
        if (bucket == NULL)
            return NULL_I64;

        bucket->key = key;
        bucket->val = val;
        bucket->next = NULL;
        ht->table[index] = bucket;
        ht->count++;

        return val;
    }

    while (bucket != NULL) {
        if (cmp(bucket->key, key, seed) == 0)
            return bucket->val;

        bucket = bucket->next;
    }

    // Key does not exist, insert a new bucket
    bucket = (bucket_p)heap_alloc(sizeof(struct bucket_t));
    if (bucket == NULL)
        return NULL_I64;

    bucket->key = key;
    bucket->val = val;
    bucket->next = ht->table[index];
    ht->table[index] = bucket;
    ht->count++;

    return val;
}

i64_t ht_bk_insert_par(ht_bk_p ht, i64_t key, i64_t val) {
    i64_t index;
    bucket_p new_bucket, current_bucket, b;

    index = key % ht->size;

    new_bucket = (bucket_p)heap_alloc(sizeof(struct bucket_t));
    if (new_bucket == NULL)
        return NULL_I64;

    new_bucket->key = key;
    new_bucket->val = val;

    for (;;) {
        current_bucket = __atomic_load_n(&ht->table[index], __ATOMIC_ACQUIRE);
        b = current_bucket;

        while (b != NULL) {
            if (b->key == key) {
                heap_free(new_bucket);
                return b->val;  // Key already exists
            }

            b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE);
        }

        new_bucket->next = current_bucket;
        if (__atomic_compare_exchange_n(&ht->table[index], &current_bucket, new_bucket, 1, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            __atomic_fetch_add(&ht->count, 1, __ATOMIC_RELAXED);
            return val;
        }
    }
}

i64_t ht_bk_insert_with_par(ht_bk_p ht, i64_t key, i64_t val, hash_f hash, cmp_f cmp, raw_p seed) {
    i64_t index;
    bucket_p new_bucket, current_bucket, b;

    index = hash(key, seed) % ht->size;

    new_bucket = (bucket_p)heap_alloc(sizeof(struct bucket_t));
    if (new_bucket == NULL)
        return NULL_I64;

    new_bucket->key = key;
    new_bucket->val = val;

    for (;;) {
        current_bucket = __atomic_load_n(&ht->table[index], __ATOMIC_ACQUIRE);
        b = current_bucket;

        while (b != NULL) {
            if (cmp(b->key, key, seed) == 0) {
                heap_free(new_bucket);
                return b->val;  // Key already exists
            }

            b = __atomic_load_n(&b->next, __ATOMIC_ACQUIRE);
        }

        new_bucket->next = current_bucket;
        if (__atomic_compare_exchange_n(&ht->table[index], &current_bucket, new_bucket, 1, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
            __atomic_fetch_add(&ht->count, 1, __ATOMIC_RELAXED);
            return val;
        }
    }
}

i64_t ht_bk_get(ht_bk_p ht, i64_t key) {
    i64_t index = key % ht->size;
    bucket_p current = ht->table[index];

    while (current != NULL) {
        if (current->key == key)
            return current->val;

        current = current->next;
    }

    return NULL_I64;
}

i64_t hash_kmh(i64_t key, raw_p seed) {
    UNUSED(seed);
    return (key * 6364136223846793005ull) >> 32;
}

i64_t hash_fnv1a(i64_t key, raw_p seed) {
    UNUSED(seed);
    i64_t hash = 14695981039346656037ull;
    i32_t i;

    for (i = 0; i < 8; i++) {
        u8_t byte = (key >> (i * 8)) & 0xff;
        hash ^= byte;
        hash *= 1099511628211ull;
    }

    return hash;
}

i64_t hash_murmur3(i64_t key, raw_p seed) {
    UNUSED(seed);
    i64_t hash = key;

    // Use a 64-bit mix function
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;

    return hash;
}

i64_t hash_guid(i64_t a, raw_p seed) {
    UNUSED(seed);
    guid_t *g = (guid_t *)a;
    i64_t hash = 0xcbf29ce484222325ull;  // FNV-1a offset basis

    // Hash each byte of the GUID
    for (size_t i = 0; i < sizeof(guid_t); i++) {
        hash ^= ((u8_t *)g)[i];
        hash *= 1099511628211ull;  // FNV-1a prime
    }

    // Final mixing step
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ull;
    hash ^= hash >> 33;

    return hash;
}

i64_t hash_i64(i64_t a, raw_p seed) {
    UNUSED(seed);
    return (i64_t)a;
}

i64_t hash_obj(i64_t a, raw_p seed) {
    UNUSED(seed);
    return hash_index_obj((obj_p)a);
}

i64_t hash_cmp_i64(i64_t a, i64_t b, raw_p seed) {
    UNUSED(seed);
    return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

i64_t hash_cmp_obj(i64_t a, i64_t b, raw_p seed) {
    UNUSED(seed);
    return cmp_obj((obj_p)a, (obj_p)b);
}

i64_t hash_cmp_guid(i64_t a, i64_t b, raw_p seed) {
    UNUSED(seed);
    guid_t *g1 = (guid_t *)a, *g2 = (guid_t *)b;
    return memcmp(g1, g2, sizeof(guid_t));
}
