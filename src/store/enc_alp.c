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

/* ALP (spec "Encodings", code 6): a float vector as decimal integers
 * ints[i] with one exponent / factor pair (e, f) per vector, decoded by
 * the fixed expression (double)ints[i] * F10[f] * IF10[e].  The encoder
 * keeps a value only if that expression gives back its exact bits;
 * everything else (NaN, inf, -0.0, out of range, not decimal) is an
 * exception stored raw.
 *
 * This file is compiled with strict IEEE arithmetic in every build
 * profile (STRICT_FP_CFLAGS, Makefile): reassociation or contraction would
 * change the decoded bits between a release and a debug build. */
#include "store/enc.h"
#include <rayforce.h>
#include <float.h>
#include <stdlib.h>          /* qsort (ALP_RD) */
#include <string.h>

#if defined(__FAST_MATH__) || defined(__ASSOCIATIVE_MATH__) || defined(__NO_SIGNED_ZEROS__) || \
    (defined(__GCC_IEC_559) && __GCC_IEC_559 < 2)
#  error "src/store/enc_alp.c needs strict IEEE flags (STRICT_FP_CFLAGS, Makefile)"
#endif
_Static_assert(FLT_EVAL_METHOD == 0, "ALP needs FLT_EVAL_METHOD 0 (no x87 excess precision)");
#if defined(__clang__)
/* Clang defines none of the macros above; the Makefile rule is what
 * protects it, and these pragmas pin the same rules in the source. */
#  pragma clang fp contract(off) reassociate(off)
#endif

/* Fixed tables of 10^k and 10^-k in the element's precision (spec "ALP"). */
static const double F10[19] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
                                1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18 };
static const double IF10[19] = { 1e0, 1e-1, 1e-2, 1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9, 1e-10,
                                 1e-11, 1e-12, 1e-13, 1e-14, 1e-15, 1e-16, 1e-17, 1e-18 };
static const float F10F[19] = { 1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f,
                                1e11f, 1e12f, 1e13f, 1e14f, 1e15f, 1e16f, 1e17f, 1e18f };
static const float IF10F[19] = { 1e0f, 1e-1f, 1e-2f, 1e-3f, 1e-4f, 1e-5f, 1e-6f, 1e-7f, 1e-8f, 1e-9f, 1e-10f,
                                 1e-11f, 1e-12f, 1e-13f, 1e-14f, 1e-15f, 1e-16f, 1e-17f, 1e-18f };

static inline int64_t p8(int64_t x) { return (x + 7) & ~(int64_t)7; }
/* The ints child is I64 (spec erratum E-P5-1): FOR_BP takes its base and
 * range in signed order, so mixed-sign decimals pack at their real width. */
static const ray_enc_kind_t K_I64 = { 64, true, false };
static inline bool alp_t_ok(int T) { return T == 32 || T == 64; }

/* Decode of one integer (the spec's expression). */
static uint64_t alp_dec1(int64_t n, int T, int e, int f) {
    if (T == 64) { double b = (double)n * F10[f] * IF10[e]; uint64_t u; memcpy(&u, &b, 8); return u; }
    float b = (float)n * F10F[f] * IF10F[e]; uint32_t u; memcpy(&u, &b, 4); return u;
}

/* The value's integer in (e, f), or false when it is an exception. */
static bool alp_enc1(uint64_t bits, int T, int e, int f, int64_t* out) {
    int64_t r;
    if (T == 64) {
        double x; memcpy(&x, &bits, 8);
        double d = x * F10[e] * IF10[f];
        if (!(d > -9.0e18 && d < 9.0e18)) return false;            /* NaN, inf, out of range */
        r = (int64_t)(d < 0 ? d - 0.5 : d + 0.5);
    } else {
        uint32_t b32 = (uint32_t)bits; float x; memcpy(&x, &b32, 4);
        float d = x * F10F[e] * IF10F[f];
        if (!(d > -9.0e18f && d < 9.0e18f)) return false;
        r = (int64_t)(d < 0 ? d - 0.5f : d + 0.5f);
        bits = b32;                                                 /* the lane's 32 bits only */
    }
    if (alp_dec1(r, T, e, f) != bits) return false;                /* keep only bit-exact values */
    *out = r;
    return true;
}

typedef struct { int n_exc; uint16_t pos[RAY_ENC_VEC]; uint64_t ints[RAY_ENC_VEC]; } alp_plan_t;

/* ints for every row and the exception positions; an exception row holds
 * the vector's first non-exception integer (0 if none). */
static void alp_plan(const uint64_t* v, int n, int T, int e, int f, alp_plan_t* p) {
    int64_t first = 0; bool have = false;
    p->n_exc = 0;
    for (int i = 0; i < n; i++) {
        int64_t r;
        if (alp_enc1(v[i], T, e, f, &r)) {
            if (!have) { first = r; have = true; }
            p->ints[i] = (uint64_t)r;
        } else {
            p->pos[p->n_exc++] = (uint16_t)i;
            p->ints[i] = UINT64_MAX;                                /* marked; set below */
        }
    }
    for (int k = 0; k < p->n_exc; k++) p->ints[p->pos[k]] = (uint64_t)first;
}

static int64_t alp_plan_size(const alp_plan_t* p, int n, int T) {
    ray_enc_choice_t c = ray_enc_choose(p->ints, n, K_I64, RAY_ENC_SLOT_ALP_INTS, 1, NULL);
    return 8 + c.bytes + p8(2LL * p->n_exc) + p8((int64_t)p->n_exc * (T / 8));
}

/* alp_plan_t is about 10 KB: heap, not stack (this runs on pool workers). */
int64_t ray_enc_alp_size(const uint64_t* v, int n, int T, const ray_enc_alp_cands_t* c, int* best) {
    if (!c || c->n <= 0 || c->n > RAY_ENC_ALP_CANDS || n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T)) return -1;
    for (int i = 0; i < c->n; i++) if (c->c[i].e > 18 || c->c[i].f > c->c[i].e) return -1;
    alp_plan_t* p = ray_alloc_raw(sizeof *p);
    if (!p) return -1;
    int64_t bs = -1;
    for (int i = 0; i < c->n; i++) {
        alp_plan(v, n, T, c->c[i].e, c->c[i].f, p);
        int64_t s = alp_plan_size(p, n, T);
        if (bs < 0 || s < bs) { bs = s; *best = i; }
    }
    ray_free_raw(p);
    return bs;
}

bool ray_enc_alp_write(const uint64_t* v, int n, int T, uint8_t e, uint8_t f, ray_enc_buf_t* out) {
    if (n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T) || e > 18 || f > e) return false;
    alp_plan_t* p = ray_alloc_raw(sizeof *p);
    if (!p) { out->oom = true; return false; }
    alp_plan(v, n, T, e, f, p);
    size_t at = out->len;
    uint16_t ne = (uint16_t)p->n_exc;
    uint8_t h[8] = { e, f, 0, 0, 0, 0, 0, 0 };
    memcpy(h + 2, &ne, 2);
    ray_enc_choice_t c;
    bool ok = ray_enc_buf_put(out, h, 8) &&
              ray_enc_write(p->ints, n, K_I64, RAY_ENC_SLOT_ALP_INTS, 1, NULL, out, &c) &&
              ray_enc_buf_put(out, p->pos, 2u * ne) && ray_enc_buf_pad8(out);
    for (int k = 0; ok && k < ne; k++) ok = ray_enc_buf_put(out, &v[p->pos[k]], (size_t)T / 8);
    ok = ok && ray_enc_buf_pad8(out);
    ray_free_raw(p);
    if (!ok) out->len = at;                                         /* no partial body */
    return ok;
}

static bool zero_bytes(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) if (p[i]) return false; return true; }

int64_t ray_enc_alp_read(const uint8_t* p, size_t avail, int n, int T, uint64_t* out) {
    if (n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T) || avail < 8) return -1;
    uint8_t e = p[0], f = p[1];
    uint16_t ne; memcpy(&ne, p + 2, 2);
    uint32_t rsv; memcpy(&rsv, p + 4, 4);
    if (e > 18 || f > e || ne > n || rsv) return -1;
    int64_t a = ray_enc_read(p + 8, avail - 8, 0, 0, n, K_I64, RAY_ENC_SLOT_ALP_INTS, 1, NULL, out);   /* ints, decoded in place below */
    if (a < 0) return -1;
    size_t at = 8 + (size_t)a, esz = (size_t)T / 8;
    size_t pb = (size_t)p8(2LL * ne), rb = (size_t)p8((int64_t)ne * (int64_t)esz);
    if (avail - at < pb + rb) return -1;
    const uint8_t* pos = p + at;
    if (!zero_bytes(pos + 2u * ne, pb - 2u * ne)) return -1;
    const uint8_t* raw = pos + pb;
    if (!zero_bytes(raw + (size_t)ne * esz, rb - (size_t)ne * esz)) return -1;
    for (int i = 0; i < n; i++) out[i] = alp_dec1((int64_t)out[i], T, e, f);
    int prev = -1;
    for (int k = 0; k < ne; k++) {
        uint16_t q; memcpy(&q, pos + 2 * k, 2);
        if ((int)q <= prev || q >= n) return -1;               /* strictly ascending, below n */
        prev = q;
        uint64_t x = 0; memcpy(&x, raw + (size_t)k * esz, esz);
        out[q] = x;
    }
    return (int64_t)(at + pb + rb);
}

/* ---- candidates: 32-value samples, the five most frequent winners ---- */
/* The (e, f) with the fewest exceptions on the sample, then the narrowest
 * integer range; scanned in increasing e, then f, replacing only on a
 * strictly better pair, so ties keep the smaller pair. */
static void alp_best_ef(const uint64_t* s, int ns, int T, int* be, int* bf, int* bexc) {
    int best_exc = ns + 1, best_w = 65, e_best = 0, f_best = 0;
    for (int e = 0; e <= 18; e++)
        for (int f = 0; f <= e; f++) {
            int exc = 0; bool have = false; int64_t mn = 0, mx = 0;
            for (int i = 0; i < ns; i++) {
                int64_t r;
                if (!alp_enc1(s[i], T, e, f, &r)) { exc++; continue; }
                if (!have || r < mn) mn = r;
                if (!have || r > mx) mx = r;
                have = true;
            }
            int w = have ? ray_enc_bitlen((uint64_t)mx - (uint64_t)mn) : 0;
            if (exc < best_exc || (exc == best_exc && w < best_w)) {
                best_exc = exc; best_w = w; e_best = e; f_best = f;
            }
        }
    *be = e_best; *bf = f_best; *bexc = best_exc;
}

void ray_enc_alp_train(const uint64_t* bits, int64_t n, int T, ray_enc_alp_cands_t* out) {
    uint32_t count[19][19];
    memset(count, 0, sizeof count);
    memset(out, 0, sizeof *out);
    for (int64_t s = 0; s < n; s += RAY_ENC_VEC) {
        int vn = (int)(n - s < RAY_ENC_VEC ? n - s : RAY_ENC_VEC);
        uint64_t smp[RAY_ENC_VEC / 32];
        int ns = 0;
        for (int i = 0; i < vn; i += 32) smp[ns++] = bits[s + i];
        int e, f, exc;
        alp_best_ef(smp, ns, T, &e, &f, &exc);
        if (exc < ns) count[e][f]++;
    }
    for (int k = 0; k < RAY_ENC_ALP_CANDS; k++) {
        uint32_t bc = 0; int be = -1, bf = -1;
        for (int e = 0; e <= 18; e++)
            for (int f = 0; f <= e; f++)
                if (count[e][f] > bc) { bc = count[e][f]; be = e; bf = f; }   /* ties: smaller e, f first */
        if (be < 0) break;
        out->c[out->n].e = (uint8_t)be; out->c[out->n].f = (uint8_t)bf; out->n++;
        count[be][bf] = 0;
    }
}
