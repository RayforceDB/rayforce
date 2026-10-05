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

#include "store/enc.h"
#include <string.h>

size_t ray_enc_packed_bytes(int w) { return (size_t)RAY_ENC_VEC * (size_t)w / 8; }

int ray_enc_bitlen(uint64_t x) { return x ? 64 - __builtin_clzll(x) : 0; }

static inline uint64_t word_get(const uint8_t* p, int T, size_t idx) {
    switch (T) {
    case 8:  return p[idx];
    case 16: { uint16_t v; memcpy(&v, p + idx * 2, 2); return v; }
    case 32: { uint32_t v; memcpy(&v, p + idx * 4, 4); return v; }
    default: { uint64_t v; memcpy(&v, p + idx * 8, 8); return v; }
    }
}

static inline void word_put(uint8_t* p, int T, size_t idx, uint64_t v) {
    switch (T) {
    case 8:  p[idx] = (uint8_t)v; break;
    case 16: { uint16_t x = (uint16_t)v; memcpy(p + idx * 2, &x, 2); break; }
    case 32: { uint32_t x = (uint32_t)v; memcpy(p + idx * 4, &x, 4); break; }
    default: memcpy(p + idx * 8, &v, 8); break;
    }
}

static inline uint64_t tmask(int T) { return T == 64 ? ~0ull : ((1ull << T) - 1); }

void ray_enc_pack(const uint64_t* u, int T, int w, uint8_t* out) {
    if (w == 0) return;
    const int L = RAY_ENC_VEC / T;
    memset(out, 0, ray_enc_packed_bytes(w));
    const uint64_t vmask = w == 64 ? ~0ull : ((1ull << w) - 1);
    const uint64_t wm = tmask(T);
    for (int l = 0; l < L; l++) {
        int bit = 0;                               /* offset in lane's stream */
        for (int r = 0; r < T; r++, bit += w) {
            uint64_t x = u[l + r * L] & vmask;
            int k = bit / T, off = bit % T;
            size_t i0 = (size_t)k * (size_t)L + (size_t)l;
            word_put(out, T, i0, (word_get(out, T, i0) | (x << off)) & wm);
            if (off + w > T) {
                size_t i1 = (size_t)(k + 1) * (size_t)L + (size_t)l;
                word_put(out, T, i1, (word_get(out, T, i1) | (x >> (T - off))) & wm);
            }
        }
    }
}

void ray_enc_unpack(const uint8_t* in, int T, int w, uint64_t* u) {
    if (w == 0) { memset(u, 0, sizeof(uint64_t) * RAY_ENC_VEC); return; }
    const int L = RAY_ENC_VEC / T;
    const uint64_t vmask = w == 64 ? ~0ull : ((1ull << w) - 1);
    for (int l = 0; l < L; l++) {
        int bit = 0;
        for (int r = 0; r < T; r++, bit += w) {
            int k = bit / T, off = bit % T;
            uint64_t x = word_get(in, T, (size_t)k * (size_t)L + (size_t)l) >> off;
            if (off + w > T)
                x |= word_get(in, T, (size_t)(k + 1) * (size_t)L + (size_t)l) << (T - off);
            u[l + r * L] = x & vmask;
        }
    }
}
