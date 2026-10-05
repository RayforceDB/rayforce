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
#include <rayforce.h>
#include <string.h>

static inline uint64_t tmask(int T) { return T == 64 ? ~0ull : ((1ull << T) - 1); }
static inline int64_t pad8(int64_t x) { return (x + 7) & ~(int64_t)7; }

/* ---- buffer ---- */
void ray_enc_buf_free(ray_enc_buf_t* b) {
    if (b->p) ray_free_raw(b->p);
    b->p = NULL; b->len = b->cap = 0;
}

bool ray_enc_buf_put(ray_enc_buf_t* b, const void* src, size_t n) {
    if (b->oom) return false;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        void* p = ray_realloc_raw(b->p, cap);
        if (!p) { b->oom = true; return false; }
        b->p = p; b->cap = cap;
    }
    if (src) memcpy(b->p + b->len, src, n); else memset(b->p + b->len, 0, n);
    b->len += n;
    return true;
}

bool ray_enc_buf_pad8(ray_enc_buf_t* b) {
    size_t want = (size_t)pad8((int64_t)b->len);
    return ray_enc_buf_put(b, NULL, want - b->len);
}

/* ---- sizes ---- */
int64_t ray_enc_size_raw(int n, int T) { return pad8((int64_t)n * T / 8); }
int64_t ray_enc_size_const(void)       { return 8; }
int64_t ray_enc_size_for(int w)        { return 8 + (int64_t)ray_enc_packed_bytes(w); }
int64_t ray_enc_size_delta(int T, int w) {
    return 8 + 8 * (int64_t)(RAY_ENC_VEC / T) + (int64_t)ray_enc_packed_bytes(w);
}

int ray_enc_for_width(const uint64_t* v, int n, ray_enc_kind_t k, uint64_t* base) {
    ray_enc_stats_t s;
    ray_enc_stats(v, n, k, &s);
    *base = s.min;
    uint64_t range = (s.max - s.min) & tmask(k.T);
    return ray_enc_bitlen(range);
}

/* d[i] = (bits(v[i]) - bits(v[i-L])) mod 2^T for i >= L over the vector
 * padded to 1024 with its last value; dbase = unsigned min of d[L..1023];
 * width = bit length of the unsigned max of (d - dbase) mod 2^T. */
static void delta_fill(const uint64_t* v, int n, int T, uint64_t* d) {
    const int L = RAY_ENC_VEC / T;
    const uint64_t m = tmask(T);
    uint64_t last = n ? v[n - 1] : 0;
    for (int i = L; i < RAY_ENC_VEC; i++) {
        uint64_t a = i < n ? v[i] : last, b = (i - L) < n ? v[i - L] : last;
        d[i] = (a - b) & m;
    }
}

int ray_enc_delta_width(const uint64_t* v, int n, ray_enc_kind_t k, uint64_t* dbase) {
    const int T = k.T, L = RAY_ENC_VEC / T;
    const uint64_t m = tmask(T);
    uint64_t d[RAY_ENC_VEC];
    delta_fill(v, n, T, d);
    uint64_t mn = d[L], mx = 0;
    for (int i = L; i < RAY_ENC_VEC; i++) if (d[i] < mn) mn = d[i];
    for (int i = L; i < RAY_ENC_VEC; i++) { uint64_t r = (d[i] - mn) & m; if (r > mx) mx = r; }
    *dbase = mn;
    return ray_enc_bitlen(mx);
}
