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
static bool alp_args_ok(int n, int T, const ray_enc_alp_cands_t* c) {
    if (!c || c->n <= 0 || c->n > RAY_ENC_ALP_CANDS || n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T)) return false;
    for (int i = 0; i < c->n; i++) if (c->c[i].e > 18 || c->c[i].f > c->c[i].e) return false;
    return true;
}

int64_t ray_enc_alp_size(const uint64_t* v, int n, int T, const ray_enc_alp_cands_t* c, int* best) {
    if (!alp_args_ok(n, T, c)) return -1;
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

/* A failed read names its rule through ray_enc_read_why (ray_enc_read_fail);
 * a failed child read keeps the child's reason. */
#define ALP_FAIL(why) ray_enc_read_fail(why)

/* The body of an ALP ints child at c (avail bytes left in the ALP body)
 * when it is a FOR_BP child that ray_enc_read accepts whole at depth 1:
 * its header (reserved bytes zero, a body size that is a multiple of 8
 * inside the parent and exactly the packed size of its width <= 64); else
 * NULL, and ray_enc_read reads (or rejects) the child. */
static const uint8_t* alp_for_child(const uint8_t* c, size_t avail) {
    if (avail < 8 || c[0] != RAY_ENC_FOR_BP || c[1] > 64 || c[2] || c[3]) return NULL;
    uint32_t bb; memcpy(&bb, c + 4, 4);
    if ((bb & 7) || bb > avail - 8 || bb != (uint64_t)ray_enc_size_for(c[1])) return NULL;
    return c + 8;
}

/* ALP decode fused with the FOR_BP unpack of its ints (I64 lanes: L = 16
 * words per row, row r's field at bit r * w of its lane): base + field,
 * then alp_dec1, row by row over contiguous lanes.  The same integers and
 * the same expression as ray_enc_read followed by the alp_dec1 loop. */
static inline __attribute__((always_inline)) void alp_dec_for(const uint8_t* b, int w, int n, int T, int e, int f,
                                                              uint64_t* restrict out) {
    enum { L = RAY_ENC_VEC / 64 };
    uint64_t base; memcpy(&base, b, 8);
    const uint8_t* pk = b + 8;
    const uint64_t vm = w >= 64 ? ~0ull : (1ull << w) - 1;
    /* F64 with every integer inside [-2^51, 2^51): the integer added to
     * the pattern of 1.5 * 2^52, then 1.5 * 2^52 subtracted, is the
     * integer as a double exactly (both steps are exact), so the bits
     * are (double)n's; unlike the int64 conversion it vectorises.  The
     * multiplies are alp_dec1's, in its order. */
    const int64_t lim = (int64_t)1 << 51;
    if (T == 64 && w >= 1 && w <= 50 && (int64_t)base >= -lim && (int64_t)base < lim - (int64_t)vm) {
        const double m1 = F10[f], m2 = IF10[e], mg = 6755399441055744.0;    /* 1.5 * 2^52 */
        const uint64_t mgb = 0x4338000000000000ull;
        for (int r = 0, bit = 0; r * L < n; r++, bit += w) {
            const int k = bit / 64, off = bit % 64;
            const int nl = n - r * L < L ? n - r * L : L;
            uint64_t* o = out + (size_t)r * L;
            const uint8_t* lo = pk + (size_t)k * L * 8;
            const unsigned sl = (unsigned)(64 - off) & 63;
            const bool two = off + w > 64;
            for (int l = 0; l < nl; l++) {
                uint64_t x0, x1 = 0; memcpy(&x0, lo + 8 * l, 8);
                if (two) memcpy(&x1, lo + 8 * (l + L), 8);
                const uint64_t u = ((x0 >> off) | (two ? x1 << sl : 0)) & vm;
                const uint64_t t = base + u + mgb;
                double d; memcpy(&d, &t, 8);
                const double b = (d - mg) * m1 * m2;
                memcpy(&o[l], &b, 8);
            }
        }
        return;
    }
    for (int r = 0, bit = 0; r * L < n; r++, bit += w) {
        const int k = bit / 64, off = bit % 64;
        const int nl = n - r * L < L ? n - r * L : L;
        uint64_t* o = out + (size_t)r * L;
        const uint8_t* lo = pk + (size_t)k * L * 8;
        if (w == 0) {
            for (int l = 0; l < nl; l++) o[l] = alp_dec1((int64_t)base, T, e, f);
        } else if (off + w > 64) {
            for (int l = 0; l < nl; l++) {
                uint64_t x0, x1; memcpy(&x0, lo + 8 * l, 8); memcpy(&x1, lo + 8 * (l + L), 8);
                o[l] = alp_dec1((int64_t)(base + (((x0 >> off) | (x1 << (64 - off))) & vm)), T, e, f);
            }
        } else {
            for (int l = 0; l < nl; l++) {
                uint64_t x0; memcpy(&x0, lo + 8 * l, 8);
                o[l] = alp_dec1((int64_t)(base + ((x0 >> off) & vm)), T, e, f);
            }
        }
    }
}

int64_t ray_enc_alp_read(const uint8_t* p, size_t avail, int n, int T, uint64_t* out) {
    if (n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T)) return ALP_FAIL("ALP vector length or float width");
    if (avail < 8) return ALP_FAIL("ALP header short");
    uint8_t e = p[0], f = p[1];
    uint16_t ne; memcpy(&ne, p + 2, 2);
    uint32_t rsv; memcpy(&rsv, p + 4, 4);
    if (e > 18) return ALP_FAIL("ALP exponent above 18");
    if (f > e) return ALP_FAIL("ALP factor above the exponent");
    if (ne > n) return ALP_FAIL("ALP exception count above the vector length");
    if (rsv) return ALP_FAIL("ALP reserved word not zero");
    /* A FOR_BP ints child that ray_enc_read would accept whole (the usual
     * case) is decoded below in one fused pass; any other child is read
     * by ray_enc_read, which names its errors. */
    const uint8_t* fb = alp_for_child(p + 8, avail - 8);
    int64_t a;
    if (fb) {
        uint32_t bb; memcpy(&bb, p + 8 + 4, 4);
        a = 8 + (int64_t)bb;
    } else {
        a = ray_enc_read(p + 8, avail - 8, 0, 0, n, K_I64, RAY_ENC_SLOT_ALP_INTS, 1, NULL, out);   /* ints, decoded in place below */
        if (a < 0) return -1;
    }
    size_t at = 8 + (size_t)a, esz = (size_t)T / 8;
    size_t pb = (size_t)p8(2LL * ne), rb = (size_t)p8((int64_t)ne * (int64_t)esz);
    if (avail - at < pb + rb) return ALP_FAIL("ALP exceptions overrun the body");
    const uint8_t* pos = p + at;
    if (!zero_bytes(pos + 2u * ne, pb - 2u * ne)) return ALP_FAIL("ALP exception position padding");
    const uint8_t* raw = pos + pb;
    if (!zero_bytes(raw + (size_t)ne * esz, rb - (size_t)ne * esz)) return ALP_FAIL("ALP exception value padding");
    if (fb) {
        if (T == 64) alp_dec_for(fb, p[9], n, 64, e, f, out); else alp_dec_for(fb, p[9], n, 32, e, f, out);
    } else {
        for (int i = 0; i < n; i++) out[i] = alp_dec1((int64_t)out[i], T, e, f);
    }
    int prev = -1;
    for (int k = 0; k < ne; k++) {
        uint16_t q; memcpy(&q, pos + 2 * k, 2);
        if ((int)q <= prev || q >= n) return ALP_FAIL("ALP exception positions not ascending or past the vector");
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

/* ---- ALP_RD (spec "Encodings", code 7) ----
 * Integer arithmetic only: the bytes do not depend on the float flags. */
static const ray_enc_kind_t K_U8 = { 8, false, false };

typedef struct {
    int rb, dict_n, n_exc;
    uint16_t dict[8];
    uint64_t codes[RAY_ENC_VEC];
    uint16_t pos[RAY_ENC_VEC], lefts[RAY_ENC_VEC];
} alprd_plan_t;

static inline uint64_t alprd_mask(int T) { return T == 64 ? ~0ull : 0xFFFFFFFFull; }

static int u16_cmp(const void* a, const void* b) {
    uint16_t x = *(const uint16_t*)a, y = *(const uint16_t*)b;
    return x < y ? -1 : x > y;
}

/* The plan at one right_bits: the eight most frequent left parts as the
 * dictionary (strictly ascending), a code per row, the rest exceptions
 * (code 0, raw left part kept). */
static void alprd_plan_rb(const uint64_t* v, int n, int T, int rb, alprd_plan_t* p) {
    const uint64_t m = alprd_mask(T);
    uint16_t s[RAY_ENC_VEC];
    for (int i = 0; i < n; i++) s[i] = (uint16_t)((v[i] & m) >> rb);
    qsort(s, (size_t)n, 2, u16_cmp);
    /* the eight most frequent left parts; ties: the smaller value */
    uint16_t val[8]; int cnt[8]; int nd = 0;
    for (int i = 0; i < n;) {
        int j = i; while (j < n && s[j] == s[i]) j++;
        int c = j - i;
        int at = nd;
        while (at > 0 && cnt[at - 1] < c) at--;                       /* stable: equal counts keep order */
        if (at < 8) {
            for (int k = (nd < 8 ? nd : 7); k > at; k--) { val[k] = val[k - 1]; cnt[k] = cnt[k - 1]; }
            val[at] = s[i]; cnt[at] = c;
            if (nd < 8) nd++;
        }
        i = j;
    }
    qsort(val, (size_t)nd, 2, u16_cmp);                                /* strictly ascending */
    p->rb = rb; p->dict_n = nd; p->n_exc = 0;
    memcpy(p->dict, val, (size_t)nd * 2);
    for (int i = 0; i < n; i++) {
        uint16_t l = (uint16_t)((v[i] & m) >> rb);
        int code = -1;
        for (int k = 0; k < nd; k++) if (p->dict[k] == l) { code = k; break; }
        if (code < 0) { p->codes[i] = 0; p->pos[p->n_exc] = (uint16_t)i; p->lefts[p->n_exc++] = l; }
        else p->codes[i] = (uint64_t)code;
    }
}

static int64_t alprd_plan_size(const alprd_plan_t* p, int n) {
    ray_enc_choice_t c = ray_enc_choose(p->codes, n, K_U8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL);
    return 8 + p8(2LL * p->dict_n) + c.bytes + (int64_t)ray_enc_packed_bytes(p->rb) + 2 * p8(2LL * p->n_exc);
}

/* The smallest plan over right_bits T-1 down to T-16 (ties keep the larger
 * right_bits); -1 on OOM.  alprd_plan_t is about 12 KB: heap, not stack. */
static int64_t alprd_best(const uint64_t* v, int n, int T, alprd_plan_t* best) {
    alprd_plan_t* p = ray_alloc_raw(sizeof *p);
    if (!p) return -1;
    int64_t bs = -1;
    for (int rb = T - 1; rb >= T - 16; rb--) {
        alprd_plan_rb(v, n, T, rb, p);
        int64_t s = alprd_plan_size(p, n);
        if (bs < 0 || s < bs) { bs = s; if (best) *best = *p; }
    }
    ray_free_raw(p);
    return bs;
}

int64_t ray_enc_alprd_size(const uint64_t* v, int n, int T) {
    if (n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T)) return -1;
    return alprd_best(v, n, T, NULL);
}

bool ray_enc_alprd_write(const uint64_t* v, int n, int T, ray_enc_buf_t* out) {
    if (n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T)) return false;
    alprd_plan_t* p = ray_alloc_raw(sizeof *p);
    if (!p) { out->oom = true; return false; }
    if (alprd_best(v, n, T, p) < 0) { ray_free_raw(p); out->oom = true; return false; }   /* p unset */
    size_t at = out->len;
    const uint64_t m = alprd_mask(T), rm = (1ull << p->rb) - 1;       /* rb <= T - 1 <= 63 */
    uint16_t ne = (uint16_t)p->n_exc;
    uint8_t h[8] = { (uint8_t)p->rb, (uint8_t)p->dict_n, 0, 0, 0, 0, 0, 0 };
    memcpy(h + 2, &ne, 2);
    ray_enc_choice_t c;
    bool ok = ray_enc_buf_put(out, h, 8) && ray_enc_buf_put(out, p->dict, 2u * (size_t)p->dict_n) &&
              ray_enc_buf_pad8(out) &&
              ray_enc_write(p->codes, n, K_U8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL, out, &c);
    if (ok) {
        uint64_t* u = p->codes;                                       /* codes written: reuse for the right parts */
        for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = i < n ? (v[i] & m & rm) : 0;   /* pad rows 0 */
        size_t pk = out->len;
        ok = ray_enc_buf_put(out, NULL, ray_enc_packed_bytes(p->rb));
        if (ok) ray_enc_pack(u, T, p->rb, out->p + pk);
    }
    ok = ok && ray_enc_buf_put(out, p->pos, 2u * ne) && ray_enc_buf_pad8(out) &&
         ray_enc_buf_put(out, p->lefts, 2u * ne) && ray_enc_buf_pad8(out);
    ray_free_raw(p);
    if (!ok) out->len = at;                                           /* no partial body */
    return ok;
}

int64_t ray_enc_alprd_read(const uint8_t* p, size_t avail, int n, int T, uint64_t* out) {
    if (n <= 0 || n > RAY_ENC_VEC || !alp_t_ok(T)) return ALP_FAIL("ALP_RD vector length or float width");
    if (avail < 8) return ALP_FAIL("ALP_RD header short");
    int rb = p[0], dn = p[1];
    uint16_t ne; memcpy(&ne, p + 2, 2);
    uint32_t rsv; memcpy(&rsv, p + 4, 4);
    if (rb < T - 16 || rb > T - 1) return ALP_FAIL("ALP_RD right width outside T-16..T-1");
    if (dn < 1 || dn > 8) return ALP_FAIL("ALP_RD dictionary size outside 1..8");
    if (ne > n) return ALP_FAIL("ALP_RD exception count above the vector length");
    if (rsv) return ALP_FAIL("ALP_RD reserved word not zero");
    uint32_t lmax = 1u << (T - rb);                                    /* left parts are below this */
    size_t at = 8, db = (size_t)p8(2LL * dn);
    if (avail - at < db) return ALP_FAIL("ALP_RD dictionary short");
    uint16_t dict[8];
    for (int k = 0; k < dn; k++) {
        memcpy(&dict[k], p + at + 2 * k, 2);
        if (dict[k] >= lmax) return ALP_FAIL("ALP_RD dictionary entry wider than the left part");
        if (k && dict[k] <= dict[k - 1]) return ALP_FAIL("ALP_RD dictionary not ascending");
    }
    if (!zero_bytes(p + at + 2u * dn, db - 2u * dn)) return ALP_FAIL("ALP_RD dictionary padding");
    at += db;
    uint64_t codes[RAY_ENC_VEC];
    int64_t a = ray_enc_read(p + at, avail - at, 0, 0, n, K_U8, RAY_ENC_SLOT_ALP_RD_LEFT, 1, NULL, codes);
    if (a < 0) return -1;
    at += (size_t)a;
    size_t pk = ray_enc_packed_bytes(rb);
    size_t pb = (size_t)p8(2LL * ne);
    if (avail - at < pk + 2 * pb) return ALP_FAIL("ALP_RD body short");
    const uint8_t* pos = p + at + pk;
    const uint8_t* lefts = pos + pb;
    if (!zero_bytes(pos + 2u * ne, pb - 2u * ne)) return ALP_FAIL("ALP_RD exception position padding");
    if (!zero_bytes(lefts + 2u * ne, pb - 2u * ne)) return ALP_FAIL("ALP_RD exception left padding");
    for (int i = 0; i < n; i++) if (codes[i] >= (uint64_t)dn) return ALP_FAIL("ALP_RD left code outside the dictionary");
    int prev = -1;
    for (int k = 0; k < ne; k++) {
        uint16_t q, l; memcpy(&q, pos + 2 * k, 2); memcpy(&l, lefts + 2 * k, 2);
        if ((int)q <= prev || q >= n) return ALP_FAIL("ALP_RD exception positions not ascending or past the vector");
        /* codes[q] != 0 is corrupt: a spec clarification of "the left code is 0" (D8) */
        if (codes[q] != 0) return ALP_FAIL("ALP_RD exception row with a non-zero left code");
        if (l >= lmax) return ALP_FAIL("ALP_RD exception left part too wide");
        prev = q;
    }
    /* Every rule but the pad rows is checked: the right parts go straight
     * into out (no second 8 KB array), and a corrupt pad leaves out zeroed. */
    ray_enc_unpack(p + at, T, rb, out);
    for (int i = n; i < RAY_ENC_VEC; i++)
        if (out[i]) { memset(out, 0, sizeof *out * RAY_ENC_VEC); return ALP_FAIL("ALP_RD padding rows not zero"); }
    for (int i = 0; i < n; i++) out[i] |= (uint64_t)dict[codes[i]] << rb;
    for (int k = 0; k < ne; k++) {
        uint16_t q, l; memcpy(&q, pos + 2 * k, 2); memcpy(&l, lefts + 2 * k, 2);
        out[q] = (out[q] & ((1ull << rb) - 1)) | ((uint64_t)l << rb);          /* code 0 replaced */
    }
    return (int64_t)(at + pk + 2 * pb);
}

/* ---- the float top level ---- */
ray_enc_choice_t ray_enc_float_choose(const uint64_t* v, int n, int T, const ray_enc_dict_t* dict,
                                      const ray_enc_alp_cands_t* c, int64_t extra) {
    ray_enc_kind_t fk = { (uint8_t)T, false, true };
    int64_t raw = ray_enc_size_raw(n, T);
    ray_enc_choice_t best = { RAY_ENC_RAW, 0, raw };
    if (!alp_t_ok(T) || n <= 0 || n > RAY_ENC_VEC) return best;
    static const uint8_t ints_like[] = { RAY_ENC_CONST, RAY_ENC_RLE, RAY_ENC_DICT };
    for (size_t i = 0; i < sizeof ints_like; i++) {
        int64_t s = ray_enc_body_size(ints_like[i], v, n, fk, dict);
        if (s >= 0 && s < best.bytes) { best.enc = ints_like[i]; best.bytes = s; }
    }
    int bi;
    int64_t s = ray_enc_alp_size(v, n, T, c, &bi);                    /* -1: no candidates (or OOM) */
    if (s >= 0 && s < best.bytes) { best.enc = RAY_ENC_ALP; best.bytes = s; }
    s = ray_enc_alprd_size(v, n, T);
    if (s >= 0 && s < best.bytes) { best.enc = RAY_ENC_ALP_RD; best.bytes = s; }
    if (best.enc != RAY_ENC_RAW && (best.bytes + extra) * 8 > (raw + extra) * 7) {   /* 1/8 gain */
        best.enc = RAY_ENC_RAW; best.bytes = raw;
    }
    return best;
}

bool ray_enc_float_write(uint8_t enc, const uint64_t* v, int n, int T, const ray_enc_dict_t* dict,
                         const ray_enc_alp_cands_t* c, ray_enc_buf_t* out, ray_enc_choice_t* chosen) {
    if (!alp_t_ok(T)) return false;
    ray_enc_kind_t fk = { (uint8_t)T, false, true };
    size_t at = out->len;
    bool ok;
    if (enc == RAY_ENC_ALP) {
        int bi;
        if (!alp_args_ok(n, T, c)) return false;                        /* no candidates: not applicable */
        if (ray_enc_alp_size(v, n, T, c, &bi) < 0) { out->oom = true; return false; }   /* valid args: OOM */
        ok = ray_enc_alp_write(v, n, T, c->c[bi].e, c->c[bi].f, out);
    } else if (enc == RAY_ENC_ALP_RD) {
        ok = ray_enc_alprd_write(v, n, T, out);
    } else {
        return ray_enc_write_as(enc, v, n, fk, dict, out, chosen);
    }
    if (!ok) { out->len = at; return false; }
    if (chosen) { chosen->enc = enc; chosen->width = 0; chosen->bytes = (int64_t)(out->len - at); }
    return true;
}

int64_t ray_enc_float_read(const uint8_t* p, size_t avail, uint8_t enc, int n, int T,
                           const ray_enc_dict_t* dict, uint64_t* out) {
    if (!alp_t_ok(T)) return ALP_FAIL("float width");
    if (enc == RAY_ENC_ALP) return ray_enc_alp_read(p, avail, n, T, out);
    if (enc == RAY_ENC_ALP_RD) return ray_enc_alprd_read(p, avail, n, T, out);
    ray_enc_kind_t fk = { (uint8_t)T, false, true };
    return ray_enc_read(p, avail, enc, 0, n, fk, RAY_ENC_SLOT_TOP, 0, dict, out);
}
