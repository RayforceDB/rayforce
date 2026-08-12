/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef RAY_STR_H
#define RAY_STR_H

/*
 * str.h -- String helper functions.
 *
 * String atoms use SSO for <= 7 bytes (stored in sdata[7] with slen).
 * Long strings store data in a U8 vector pointed to by obj.
 */

#include <rayforce.h>
#include <string.h>

/* ===== Inline String Element (16 bytes) ===== */

typedef union {
    struct { uint32_t len; char     data[12]; };      /* inline: len <= 12 */
    struct { uint32_t len_; char    prefix[4];        /* pooled: len > 12  */
             uint32_t pool_off; uint32_t hash32; };
} ray_str_t;

#define RAY_STR_INLINE_MAX 12

static inline bool ray_str_is_inline(const ray_str_t* s) {
    return s->len <= RAY_STR_INLINE_MAX;
}

/* Resolve string data pointer for a ray_str_t element.
 * pool_base: base of string pool (NULL if all strings are inline) */
static inline const char* ray_str_t_ptr(const ray_str_t* s, const char* pool_base) {
    if (s->len == 0) return "";
    if (ray_str_is_inline(s)) return s->data;
    assert(pool_base != NULL && "ray_str_t_ptr: pooled string requires non-NULL pool_base");
    return pool_base + s->pool_off;
}

/* Equality: fast reject on len, then prefix, then full compare.
 * pool_a/pool_b: pool bases for elements a and b respectively (NULL if inline) */
static inline bool ray_str_t_eq(const ray_str_t* a, const char* pool_a,
                               const ray_str_t* b, const char* pool_b) {
    if (a->len != b->len) return false;
    if (a->len == 0) return true;
    if (ray_str_is_inline(a)) {
        return memcmp(a->data, b->data, a->len) == 0;
    }
    /* Slices, clones, and separately assembled tables can share one pool.
     * Equal offsets in the same immutable pool identify the same bytes. */
    if (pool_a && pool_a == pool_b && a->pool_off == b->pool_off) return true;
    /* Both pooled: check prefix first */
    if (memcmp(a->prefix, b->prefix, 4) != 0) return false;
    return memcmp(pool_a + a->pool_off, pool_b + b->pool_off, a->len) == 0;
}

/* Ordering: lexicographic, shorter string is less on prefix tie.
 * pool_a/pool_b: pool bases for elements a and b respectively (NULL if inline) */
static inline int ray_str_t_cmp(const ray_str_t* a, const char* pool_a,
                               const ray_str_t* b, const char* pool_b) {
    const char* pa = ray_str_t_ptr(a, pool_a);
    const char* pb = ray_str_t_ptr(b, pool_b);
    uint32_t min_len = a->len < b->len ? a->len : b->len;
    int r = memcmp(pa, pb, min_len);
    if (r != 0) return r;
    return (a->len > b->len) - (a->len < b->len);
}

/* Hash a ray_str_t element.  Uses FNV-1a which is self-contained and fast for
 * the typical short-to-medium strings stored in ray_str_t.
 * pool_base: pool base pointer for pooled strings (NULL when inline-only). */
static inline uint64_t ray_str_t_hash(const ray_str_t* s, const char* pool_base) {
    if (s->len == 0) return 0x9E3779B97F4A7C15ULL; /* golden ratio constant for empty */
    if (!ray_str_is_inline(s)) {
        assert(pool_base != NULL && "ray_str_t_hash: pooled string requires non-NULL pool_base");
    }
    const char* p = ray_str_is_inline(s) ? s->data : pool_base + s->pool_off;
    /* 8-byte-block multiply-mix hash: far fewer multiplies than per-byte
     * hashing for medium/long keys (URLs, titles). */
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    uint32_t len = s->len;
    uint64_t h = 0xe17a1465ULL ^ ((uint64_t)len * m);
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t k;
        memcpy(&k, p + i, 8);
        k *= m; k ^= k >> 47; k *= m;
        h ^= k; h *= m;
    }
    uint32_t rem = len - i;
    if (rem) {
        uint64_t k = 0;
        memcpy(&k, p + i, rem);   /* tail: rem<8 bytes, zero-padded high bytes */
        k *= m; k ^= k >> 47; k *= m;
        h ^= k; h *= m;
    }
    h ^= h >> 47; h *= m; h ^= h >> 47;
    return h;
}

/* Pooled descriptors have four bytes that are not needed for addressing.
 * Cache a non-zero 32-bit content hash there so joins can hash long STR keys
 * without rereading the pool on every execution.  A zero field denotes an
 * older/on-disk descriptor without a cache; computing it remains fully
 * backward compatible.  Inline strings use all 12 payload bytes and are
 * cheap enough to hash directly. */
static inline uint32_t ray_str_t_hash32(const ray_str_t* s,
                                        const char* pool_base) {
    if (!ray_str_is_inline(s) && s->hash32 != 0) return s->hash32;
    uint32_t h = (uint32_t)ray_str_t_hash(s, pool_base);
    return h != 0 ? h : 1u;
}

static inline void ray_str_t_cache_hash(ray_str_t* s, const char* pool_base) {
    if (!ray_str_is_inline(s)) s->hash32 = ray_str_t_hash32(s, pool_base);
}

#endif /* RAY_STR_H */
