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

/*
 * hash.h — Fast wyhash-based hashing for Rayforce
 *
 * Based on wyhash final version 4.2 by Wang Yi <godspeed_china@yeah.net>
 * Original: https://github.com/wangyi-fudan/wyhash
 *
 * This is free and unencumbered software released into the public domain
 * under The Unlicense (https://unlicense.org).
 * See the original repository for full license text.
 */

#ifndef RAY_HASH_H
#define RAY_HASH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---- Platform detection ------------------------------------------------- */

#if defined(__GNUC__) || defined(__INTEL_COMPILER) || defined(__clang__)
  #define RAY_HASH_LIKELY(x)   __builtin_expect(!!(x), 1)
  #define RAY_HASH_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
  #define RAY_HASH_LIKELY(x)   (x)
  #define RAY_HASH_UNLIKELY(x) (x)
#endif

#if defined(_MSC_VER) && defined(_M_X64)
  #include <intrin.h>
  #pragma intrinsic(_umul128)
#endif

#ifndef RAY_HASH_LITTLE_ENDIAN
  #if defined(RAY_OS_WINDOWS) || defined(__LITTLE_ENDIAN__) || \
      (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    #define RAY_HASH_LITTLE_ENDIAN 1
  #elif defined(__BIG_ENDIAN__) || \
        (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #define RAY_HASH_LITTLE_ENDIAN 0
  #else
    #define RAY_HASH_LITTLE_ENDIAN 1
  #endif
#endif

/* ---- Internal primitives ------------------------------------------------ */

/* 128-bit multiply: *A and *B become the low and high 64 bits of A*B */
static inline void ray__wymum(uint64_t *A, uint64_t *B) {
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)*A * *B;
    *A = (uint64_t)r;
    *B = (uint64_t)(r >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
    *A = _umul128(*A, *B, B);
#else
    uint64_t ha = *A >> 32, la = (uint32_t)*A;
    uint64_t hb = *B >> 32, lb = (uint32_t)*B;
    uint64_t rh = ha * hb, rm0 = ha * lb, rm1 = hb * la, rl = la * lb;
    uint64_t t = rl + (rm0 << 32), c = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    *A = lo;
    *B = hi;
#endif
}

/* Mix two 64-bit values via multiply-then-xor */
static inline uint64_t ray__wymix(uint64_t A, uint64_t B) {
    ray__wymum(&A, &B);
    return A ^ B;
}

/* ---- Byte readers (endian-aware) ---------------------------------------- */

static inline uint64_t ray__wyr8(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if RAY_HASH_LITTLE_ENDIAN
    return v;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(v);
#elif defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return ((v >> 56) & 0xff) | ((v >> 40) & 0xff00) |
           ((v >> 24) & 0xff0000) | ((v >> 8) & 0xff000000) |
           ((v << 8) & 0xff00000000ULL) | ((v << 24) & 0xff0000000000ULL) |
           ((v << 40) & 0xff000000000000ULL) | ((v << 56) & 0xff00000000000000ULL);
#endif
}

static inline uint64_t ray__wyr4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
#if RAY_HASH_LITTLE_ENDIAN
    return v;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(v);
#elif defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return ((v >> 24) & 0xff) | ((v >> 8) & 0xff00) |
           ((v << 8) & 0xff0000) | ((v << 24) & 0xff000000);
#endif
}

static inline uint64_t ray__wyr3(const uint8_t *p, size_t k) {
    return ((uint64_t)p[0] << 16) | ((uint64_t)p[k >> 1] << 8) | p[k - 1];
}

/* ---- Secret constants (from wyhash final4.2) ---------------------------- */

static const uint64_t ray__wyp[4] = {
    0x2d358dccaa6c78a5ULL,
    0x8bb84b93962eacc9ULL,
    0x4b33a62ed433d4a3ULL,
    0x4d5a2da51de1aa47ULL,
};

/* ---- Core: hash arbitrary bytes ----------------------------------------- */

/*
 * ray_hash_bytes -- hash a byte buffer of length `len`.
 *
 * This is the full wyhash final4.2 algorithm: ~3 cycles/8 bytes on
 * modern x86-64. Seed is fixed at 0 for deterministic, repeatable hashing
 * within a single process lifetime.
 */
/* L2: Fixed seed=0 is acceptable for in-process dataframe operations;
 * use a random seed if processing adversarial input (e.g., untrusted
 * CSV with crafted hash collisions). */
static inline uint64_t ray_hash_bytes(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t seed = 0;
    seed ^= ray__wymix(seed ^ ray__wyp[0], ray__wyp[1]);

    uint64_t a, b;
    if (RAY_HASH_LIKELY(len <= 16)) {
        if (RAY_HASH_LIKELY(len >= 4)) {
            a = (ray__wyr4(p) << 32) | ray__wyr4(p + ((len >> 3) << 2));
            b = (ray__wyr4(p + len - 4) << 32) | ray__wyr4(p + len - 4 - ((len >> 3) << 2));
        } else if (RAY_HASH_LIKELY(len > 0)) {
            a = ray__wyr3(p, len);
            b = 0;
        } else {
            a = b = 0;
        }
    } else {
        size_t i = len;
        if (RAY_HASH_UNLIKELY(i >= 48)) {
            uint64_t see1 = seed, see2 = seed;
            do {
                seed = ray__wymix(ray__wyr8(p)      ^ ray__wyp[1], ray__wyr8(p + 8)  ^ seed);
                see1 = ray__wymix(ray__wyr8(p + 16)  ^ ray__wyp[2], ray__wyr8(p + 24) ^ see1);
                see2 = ray__wymix(ray__wyr8(p + 32)  ^ ray__wyp[3], ray__wyr8(p + 40) ^ see2);
                p += 48;
                i -= 48;
            } while (RAY_HASH_LIKELY(i >= 48));
            seed ^= see1 ^ see2;
        }
        while (RAY_HASH_UNLIKELY(i > 16)) {
            seed = ray__wymix(ray__wyr8(p) ^ ray__wyp[1], ray__wyr8(p + 8) ^ seed);
            i -= 16;
            p += 16;
        }
        a = ray__wyr8(p + i - 16);
        b = ray__wyr8(p + i - 8);
    }
    a ^= ray__wyp[1];
    b ^= seed;
    ray__wymum(&a, &b);
    return ray__wymix(a ^ ray__wyp[0] ^ len, b ^ ray__wyp[1]);
}

/* ---- Convenience: hash a single int64 ----------------------------------- */

/*
 * ray_hash_i64 -- hash a 64-bit integer.
 *
 * Uses wyhash64 two-round mixing which is faster than feeding 8 bytes
 * through the generic path while retaining excellent distribution.
 */
static inline uint64_t ray_hash_i64(int64_t val) {
    uint64_t A = (uint64_t)val ^ 0x2d358dccaa6c78a5ULL;
    uint64_t B = (uint64_t)val ^ 0x8bb84b93962eacc9ULL;
    ray__wymum(&A, &B);
    return ray__wymix(A ^ 0x2d358dccaa6c78a5ULL, B ^ 0x8bb84b93962eacc9ULL);
}

/* ---- Convenience: hash a double ----------------------------------------- */

/*
 * ray_hash_f64 -- hash a 64-bit float by its bit pattern.
 *
 * Normalizes negative zero to positive zero so that -0.0 and +0.0
 * hash identically (they compare equal via ==).
 *
 * Note: different NaN bit patterns hash differently; SQL NULL is
 * handled separately at a higher level and never reaches this path.
 */
static inline uint64_t ray_hash_f64(double val) {
    uint64_t bits;
    if (val == 0.0) { uint64_t z = 0; memcpy(&val, &z, sizeof(val)); } /* normalize -0.0 → +0.0 */
    memcpy(&bits, &val, sizeof(bits));
    uint64_t A = bits ^ 0x2d358dccaa6c78a5ULL;
    uint64_t B = bits ^ 0x8bb84b93962eacc9ULL;
    ray__wymum(&A, &B);
    return ray__wymix(A ^ 0x2d358dccaa6c78a5ULL, B ^ 0x8bb84b93962eacc9ULL);
}

/* ---- Combine two hashes ------------------------------------------------- */

/*
 * ray_hash_combine -- mix two hash values into one.
 *
 * Uses the wyhash64 two-input mixer. This is order-dependent:
 * combine(a,b) != combine(b,a), which is the desired behaviour for
 * multi-column key hashing where column order matters.
 */
static inline uint64_t ray_hash_combine(uint64_t h1, uint64_t h2) {
    uint64_t A = h1 ^ 0x2d358dccaa6c78a5ULL;
    uint64_t B = h2 ^ 0x8bb84b93962eacc9ULL;
    ray__wymum(&A, &B);
    return ray__wymix(A ^ 0x2d358dccaa6c78a5ULL, B ^ 0x8bb84b93962eacc9ULL);
}

#endif /* RAY_HASH_H */
