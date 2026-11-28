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

#ifndef RAY_OPS_RAND_H
#define RAY_OPS_RAND_H

/* The engine's random stream (#1898, #1910).  One process-wide stream:
 * position p of it is splitmix64's output for counter p, so a value
 * depends only on its position, never on which thread or worker made it.
 * A user takes the next n positions with ray_rand_reserve (one atomic
 * add); every process starts at position 0, so the sequence is the same in
 * every process and on every platform.  rand and .graph.random-walk share
 * it: a call of either advances the other. */

#include <stdint.h>
#include "core/platform.h"   /* RAY_UNLIKELY */
#include "ops/hash.h"        /* ray__wymum: the portable 128-bit multiply */

#define RAY_RAND_GAMMA UINT64_C(0x9E3779B97F4A7C15)

/* The first of the next n positions of the stream */
uint64_t ray_rand_reserve(uint64_t n);

static inline uint64_t ray_rand_mix(uint64_t z) {
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

/* The stream's value at position p */
static inline uint64_t ray_rand_value(uint64_t p) {
    return ray_rand_mix((p + 1) * RAY_RAND_GAMMA);
}

/* A uniform value in [0, range) from x, the stream's value at its
 * position: the high word of x * range, rejecting the few x whose low word
 * falls under 2^64 mod range (Lemire's multiply-and-reject), which are
 * replaced from a stream of their own, seeded by x, so the result stays a
 * function of the position alone */
static inline uint64_t ray_rand_below(uint64_t x, uint64_t range) {
    uint64_t lo = x, hi = range;
    ray__wymum(&lo, &hi);
    if (RAY_UNLIKELY(lo < range)) {
        uint64_t t = (0 - range) % range;
        uint64_t s = x;
        while (lo < t) {
            s += RAY_RAND_GAMMA;
            lo = ray_rand_mix(s);
            hi = range;
            ray__wymum(&lo, &hi);
        }
    }
    return hi;
}

#endif /* RAY_OPS_RAND_H */
