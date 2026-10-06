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
    b->p = NULL; b->len = b->cap = 0; b->oom = false;
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
    *base = s.min & tmask(k.T);          /* canonical: a T-bit pattern */
    uint64_t range = (s.max - s.min) & tmask(k.T);
    return ray_enc_bitlen(range);
}

/* d[i] = (bits(v[i]) - bits(v[i-L])) mod 2^T for i >= L over the vector
 * padded to 1024 with its last value; dbase = unsigned min of d[L..1023];
 * width = bit length of the unsigned max of (d - dbase) mod 2^T. */
static void delta_fill(const uint64_t* v, int n, int T, uint64_t* d) {
    const int L = RAY_ENC_VEC / T;
    const uint64_t m = tmask(T);
    uint64_t last = n ? v[n - 1] & m : 0;
    for (int i = L; i < RAY_ENC_VEC; i++) {
        uint64_t a = i < n ? v[i] & m : last, b = (i - L) < n ? v[i - L] & m : last;
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

/* ---- slots ---- */
static const ray_enc_kind_t K_U16 = { 16, false };
static const ray_enc_kind_t K_U32 = { 32, false };

/* Kind of a child slot's values given the parent's kind. */
static ray_enc_kind_t slot_child_kind(ray_enc_slot_t child, ray_enc_kind_t parent) {
    switch (child) {
    case RAY_ENC_SLOT_RLE_ENDS: return K_U16;
    case RAY_ENC_SLOT_CODES:    return K_U32;
    default:                    return parent;   /* RLE values: enclosing kind */
    }
}

/* Is `enc` legal for (slot, depth)?  DICT also needs a dictionary. */
static bool enc_legal(uint8_t enc, ray_enc_slot_t slot, int depth, ray_enc_kind_t k,
                      const ray_enc_dict_t* dict) {
    if (depth >= RAY_ENC_DEPTH_MAX && enc != RAY_ENC_RAW && enc != RAY_ENC_FOR_BP) return false;
    switch (slot) {
    case RAY_ENC_SLOT_TOP:
        if (enc == RAY_ENC_DELTA_BP) return k.is_signed;          /* integer and temporal only */
        if (enc == RAY_ENC_DICT) return k.is_signed && dict && dict->n > 0;
        return enc == RAY_ENC_RAW || enc == RAY_ENC_CONST || enc == RAY_ENC_FOR_BP || enc == RAY_ENC_RLE;
    case RAY_ENC_SLOT_RLE_VALUES:
        return enc == RAY_ENC_RAW || enc == RAY_ENC_FOR_BP;
    case RAY_ENC_SLOT_RLE_ENDS:
        return enc == RAY_ENC_RAW || enc == RAY_ENC_FOR_BP || enc == RAY_ENC_DELTA_BP;
    case RAY_ENC_SLOT_CODES:
        return enc == RAY_ENC_RAW || enc == RAY_ENC_FOR_BP || enc == RAY_ENC_RLE;
    }
    return false;
}

/* ---- RLE and DICT helpers ---- */
static int rle_split(const uint64_t* v, int n, uint64_t* vals, uint64_t* ends) {
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || v[i] != v[i - 1]) vals[r++] = v[i];
        ends[r - 1] = (uint64_t)(i + 1);
    }
    return r;
}

static bool dict_codes(const uint64_t* v, int n, ray_enc_kind_t k,
                       const ray_enc_dict_t* d, uint64_t* codes) {
    for (int i = 0; i < n; i++) {
        uint32_t lo = 0, hi = d->n;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (ray_enc_lt(d->v[mid], v[i], k)) lo = mid + 1; else hi = mid;
        }
        if (lo >= d->n || d->v[lo] != v[i]) return false;   /* not in dictionary */
        codes[i] = lo;
    }
    return true;
}

static int64_t hdr(int depth) { return depth > 0 ? 8 : 0; }

/* Size of encoding `enc` for v, or -1 if not applicable. */
static int64_t enc_size(uint8_t enc, const uint64_t* v, int n, ray_enc_kind_t k,
                        ray_enc_slot_t slot, int depth, const ray_enc_dict_t* dict,
                        uint8_t* width) {
    uint64_t base;
    *width = 0;
    switch (enc) {
    case RAY_ENC_RAW:   return ray_enc_size_raw(n, k.T);
    case RAY_ENC_CONST: {
        ray_enc_stats_t s; ray_enc_stats(v, n, k, &s);
        return s.all_equal ? ray_enc_size_const() : -1;
    }
    case RAY_ENC_FOR_BP: { int w = ray_enc_for_width(v, n, k, &base); *width = (uint8_t)w; return ray_enc_size_for(w); }
    case RAY_ENC_DELTA_BP: { int w = ray_enc_delta_width(v, n, k, &base); *width = (uint8_t)w; return ray_enc_size_delta(k.T, w); }
    case RAY_ENC_RLE: {
        if (depth + 1 > RAY_ENC_DEPTH_MAX) return -1;
        uint64_t vals[RAY_ENC_VEC], ends[RAY_ENC_VEC];   /* locals: children recurse */
        int r = rle_split(v, n, vals, ends);
        ray_enc_kind_t vk = slot_child_kind(RAY_ENC_SLOT_RLE_VALUES, k);
        ray_enc_choice_t cv = ray_enc_choose(vals, r, vk, RAY_ENC_SLOT_RLE_VALUES, depth + 1, NULL);
        ray_enc_choice_t ce = ray_enc_choose(ends, r, K_U16, RAY_ENC_SLOT_RLE_ENDS, depth + 1, NULL);
        return 8 + cv.bytes + ce.bytes;
    }
    case RAY_ENC_DICT: {
        if (!dict) return -1;                                         /* never dereference a missing dictionary */
        uint64_t codes[RAY_ENC_VEC];
        if (!dict_codes(v, n, k, dict, codes)) return -1;
        ray_enc_choice_t cc = ray_enc_choose(codes, n, K_U32, RAY_ENC_SLOT_CODES, depth + 1, NULL);
        return cc.bytes;
    }
    }
    return -1;
}

ray_enc_choice_t ray_enc_choose(const uint64_t* v, int n, ray_enc_kind_t k,
                                ray_enc_slot_t slot, int depth,
                                const ray_enc_dict_t* dict) {
    static const uint8_t order[] = { RAY_ENC_RAW, RAY_ENC_CONST, RAY_ENC_FOR_BP,
                                     RAY_ENC_DELTA_BP, RAY_ENC_RLE, RAY_ENC_DICT };
    uint8_t w;
    int64_t raw = ray_enc_size_raw(n, k.T);
    ray_enc_choice_t best = { RAY_ENC_RAW, 0, raw };
    for (size_t i = 1; i < sizeof order; i++) {
        uint8_t e = order[i];
        if (!enc_legal(e, slot, depth, k, dict)) continue;
        int64_t sz = enc_size(e, v, n, k, slot, depth, dict, &w);
        if (sz >= 0 && sz < best.bytes) { best.enc = e; best.width = w; best.bytes = sz; }
    }
    if (best.enc != RAY_ENC_RAW && best.bytes * 8 > raw * 7) {   /* need >= 1/8 gain */
        best.enc = RAY_ENC_RAW; best.width = 0; best.bytes = raw;
    }
    best.bytes += hdr(depth);
    return best;
}

/* ---- writers ---- */
static bool put_u64(ray_enc_buf_t* b, uint64_t x) { return ray_enc_buf_put(b, &x, 8); }

static bool write_raw(const uint64_t* v, int n, int T, ray_enc_buf_t* b) {
    for (int i = 0; i < n; i++) if (!ray_enc_buf_put(b, &v[i], (size_t)T / 8)) return false;
    return ray_enc_buf_pad8(b);
}

static bool write_packed(const uint64_t* u, int T, int w, ray_enc_buf_t* b) {
    size_t at = b->len;
    if (!ray_enc_buf_put(b, NULL, ray_enc_packed_bytes(w))) return false;
    ray_enc_pack(u, T, w, b->p + at);
    return true;
}

static bool write_body(uint8_t enc, uint8_t w, const uint64_t* v, int n, ray_enc_kind_t k,
                       ray_enc_slot_t slot, int depth, const ray_enc_dict_t* dict,
                       ray_enc_buf_t* b) {
    const uint64_t m = tmask(k.T);
    switch (enc) {
    case RAY_ENC_RAW:   return write_raw(v, n, k.T, b);
    case RAY_ENC_CONST: return write_raw(v, 1, k.T, b);
    case RAY_ENC_FOR_BP: {
        uint64_t base; (void)ray_enc_for_width(v, n, k, &base);
        base &= m;
        uint64_t u[RAY_ENC_VEC];
        for (int i = 0; i < RAY_ENC_VEC; i++) u[i] = i < n ? (((v[i] & m) - base) & m) : 0;  /* pad: base */
        return put_u64(b, base) && write_packed(u, k.T, w, b);
    }
    case RAY_ENC_DELTA_BP: {
        const int L = RAY_ENC_VEC / k.T;
        uint64_t dbase; (void)ray_enc_delta_width(v, n, k, &dbase);
        uint64_t d[RAY_ENC_VEC], r[RAY_ENC_VEC];
        delta_fill(v, n, k.T, d);
        uint64_t last = n ? v[n - 1] & m : 0;
        dbase &= m;
        if (!put_u64(b, dbase)) return false;
        for (int l = 0; l < L; l++) if (!put_u64(b, (l < n ? v[l] & m : last))) return false;
        for (int i = 0; i < RAY_ENC_VEC; i++) r[i] = i < L ? 0 : ((d[i] - dbase) & m);
        return write_packed(r, k.T, w, b);
    }
    case RAY_ENC_RLE: {
        uint64_t vals[RAY_ENC_VEC], ends[RAY_ENC_VEC];
        int nr = rle_split(v, n, vals, ends);
        uint32_t hdrw[2] = { (uint32_t)nr, 0 };
        ray_enc_choice_t c;
        ray_enc_kind_t vk = slot_child_kind(RAY_ENC_SLOT_RLE_VALUES, k);
        return ray_enc_buf_put(b, hdrw, 8)
            && ray_enc_write(vals, nr, vk, RAY_ENC_SLOT_RLE_VALUES, depth + 1, NULL, b, &c)
            && ray_enc_write(ends, nr, K_U16, RAY_ENC_SLOT_RLE_ENDS, depth + 1, NULL, b, &c);
    }
    case RAY_ENC_DICT: {
        if (!dict) return false;                                      /* never dereference a missing dictionary */
        uint64_t codes[RAY_ENC_VEC];
        if (!dict_codes(v, n, k, dict, codes)) return false;
        ray_enc_choice_t c;
        return ray_enc_write(codes, n, K_U32, RAY_ENC_SLOT_CODES, depth + 1, NULL, b, &c);
    }
    }
    return false;
}

bool ray_enc_write(const uint64_t* v, int n, ray_enc_kind_t k,
                   ray_enc_slot_t slot, int depth, const ray_enc_dict_t* dict,
                   ray_enc_buf_t* out, ray_enc_choice_t* chosen) {
    if (n < 0 || n > RAY_ENC_VEC) return false;
    ray_enc_choice_t c = ray_enc_choose(v, n, k, slot, depth, dict);
    size_t at = out->len;
    if (depth > 0) {
        uint8_t h[8] = { c.enc, c.width, 0, 0, 0, 0, 0, 0 };
        if (!ray_enc_buf_put(out, h, 8)) { out->len = at; return false; }
    }
    size_t body_at = out->len;
    if (!write_body(c.enc, c.width, v, n, k, slot, depth, dict, out)) { out->len = at; return false; }
    if (depth > 0) {
        uint32_t bb = (uint32_t)(out->len - body_at);
        memcpy(out->p + at + 4, &bb, 4);
    }
    if ((int64_t)(out->len - at) != c.bytes) { out->len = at; return false; }   /* size model and writer agree */
    if (chosen) *chosen = c;
    return true;
}

bool ray_enc_write_as(uint8_t enc, const uint64_t* v, int n, ray_enc_kind_t k,
                      const ray_enc_dict_t* dict, ray_enc_buf_t* out, ray_enc_choice_t* chosen) {
    if (n < 0 || n > RAY_ENC_VEC) return false;
    if (enc > RAY_ENC_DICT || !enc_legal(enc, RAY_ENC_SLOT_TOP, 0, k, dict)) return false;
    uint8_t w;
    int64_t sz = enc_size(enc, v, n, k, RAY_ENC_SLOT_TOP, 0, dict, &w);
    if (sz < 0) return false;
    size_t at = out->len;
    if (!write_body(enc, w, v, n, k, RAY_ENC_SLOT_TOP, 0, dict, out)) { out->len = at; return false; }
    if ((int64_t)(out->len - at) != sz) { out->len = at; return false; }     /* size model and writer agree */
    if (chosen) { chosen->enc = enc; chosen->width = w; chosen->bytes = sz; }
    return true;
}

/* ---- readers ---- */
/* The reason of the last failed ray_enc_read on this thread, so a corrupt
 * file's error names the rule that fired.  Not format. */
static _Thread_local const char* rd_why;
const char* ray_enc_read_why(void) { return rd_why ? rd_why : "unknown"; }
#define RD_FAIL(why) ((void)(rd_why = (why)), -1)

static bool get_raw(const uint8_t* p, size_t avail, int n, int T, uint64_t* out, int64_t* used) {
    int64_t sz = ray_enc_size_raw(n, T);
    if ((size_t)sz > avail) { rd_why = "RAW body short"; return false; }
    for (int i = 0; i < n; i++) {
        uint64_t x = 0; memcpy(&x, p + (size_t)i * (size_t)T / 8, (size_t)T / 8); out[i] = x;
    }
    for (int64_t i = (int64_t)n * T / 8; i < sz; i++) if (p[i]) { rd_why = "RAW padding not zero"; return false; }   /* zero padding */
    *used = sz;
    return true;
}

static int64_t read_body(const uint8_t* p, size_t avail, uint8_t enc, uint8_t w, int n,
                         ray_enc_kind_t k, ray_enc_slot_t slot, int depth,
                         const ray_enc_dict_t* dict, uint64_t* out) {
    const uint64_t m = tmask(k.T);
    int64_t used;
    if (!enc_legal(enc, slot, depth, k, dict)) return RD_FAIL("encoding illegal in this slot or depth");
    if (w != 0 && enc != RAY_ENC_FOR_BP && enc != RAY_ENC_DELTA_BP) return RD_FAIL("width on a non-packed encoding");   /* width only for BP */
    switch (enc) {
    case RAY_ENC_RAW:
        return get_raw(p, avail, n, k.T, out, &used) ? used : -1;
    case RAY_ENC_CONST: {
        uint64_t x;
        if (!get_raw(p, avail, 1, k.T, &x, &used)) return -1;
        for (int i = 0; i < n; i++) out[i] = x;
        return used;
    }
    case RAY_ENC_FOR_BP: {
        if (w > k.T) return RD_FAIL("FOR_BP width above the type width");
        if (avail < (size_t)ray_enc_size_for(w)) return RD_FAIL("FOR_BP body short");
        uint64_t base; memcpy(&base, p, 8);
        if (base & ~m) return RD_FAIL("FOR_BP base above the type width");
        uint64_t u[RAY_ENC_VEC];
        ray_enc_unpack(p + 8, k.T, w, u);
        for (int i = 0; i < n; i++) out[i] = (base + u[i]) & m;
        return ray_enc_size_for(w);
    }
    case RAY_ENC_DELTA_BP: {
        const int L = RAY_ENC_VEC / k.T;
        if (w > k.T) return RD_FAIL("DELTA_BP width above the type width");
        if (avail < (size_t)ray_enc_size_delta(k.T, w)) return RD_FAIL("DELTA_BP body short");
        uint64_t dbase; memcpy(&dbase, p, 8);
        if (dbase & ~m) return RD_FAIL("DELTA_BP base above the type width");
        uint64_t full[RAY_ENC_VEC], r[RAY_ENC_VEC];
        for (int l = 0; l < L; l++) { memcpy(&full[l], p + 8 + 8 * (size_t)l, 8); if (full[l] & ~m) return RD_FAIL("DELTA_BP head above the type width"); }
        ray_enc_unpack(p + 8 + 8 * (size_t)L, k.T, w, r);
        for (int i = 0; i < L; i++) if (r[i]) return RD_FAIL("DELTA_BP placeholder not zero");   /* placeholders are zero */
        for (int i = L; i < RAY_ENC_VEC; i++) full[i] = (full[i - L] + dbase + r[i]) & m;
        memcpy(out, full, sizeof(uint64_t) * (size_t)n);
        return ray_enc_size_delta(k.T, w);
    }
    case RAY_ENC_RLE: {
        if (avail < 8) return RD_FAIL("RLE header short");
        uint32_t nr; memcpy(&nr, p, 4);
        uint32_t rsv; memcpy(&rsv, p + 4, 4);
        if (rsv) return RD_FAIL("RLE reserved word not zero");
        if (nr == 0 || nr > (uint32_t)n) return RD_FAIL("RLE run count out of range");
        uint64_t vals[RAY_ENC_VEC], ends[RAY_ENC_VEC];
        ray_enc_kind_t vk = slot_child_kind(RAY_ENC_SLOT_RLE_VALUES, k);
        int64_t a = ray_enc_read(p + 8, avail - 8, 0, 0, (int)nr, vk, RAY_ENC_SLOT_RLE_VALUES, depth + 1,
                                 NULL, vals);
        if (a < 0) return -1;
        int64_t e = ray_enc_read(p + 8 + a, avail - 8 - (size_t)a, 0, 0, (int)nr, K_U16, RAY_ENC_SLOT_RLE_ENDS, depth + 1, NULL, ends);
        if (e < 0) return -1;
        uint64_t prev = 0;
        for (uint32_t i = 0; i < nr; i++) {
            if (ends[i] <= prev || ends[i] > (uint64_t)n) return RD_FAIL("RLE run ends not ascending or past the vector");   /* strictly ascending */
            for (uint64_t j = prev; j < ends[i]; j++) out[j] = vals[i];
            prev = ends[i];
        }
        if (prev != (uint64_t)n) return RD_FAIL("RLE last run end is not the vector length");                           /* last end is n */
        return 8 + a + e;
    }
    case RAY_ENC_DICT: {
        if (!dict) return RD_FAIL("DICT without a dictionary");                                         /* never dereference a missing dictionary */
        uint64_t codes[RAY_ENC_VEC];
        int64_t c = ray_enc_read(p, avail, 0, 0, n, K_U32, RAY_ENC_SLOT_CODES, depth + 1, NULL, codes);
        if (c < 0) return -1;
        for (int i = 0; i < n; i++) { if (codes[i] >= dict->n) return RD_FAIL("DICT code outside the dictionary"); out[i] = dict->v[codes[i]]; }
        return c;
    }
    }
    return RD_FAIL("unknown encoding");
}

int64_t ray_enc_read(const uint8_t* p, size_t avail, uint8_t enc, uint8_t width,
                     int n, ray_enc_kind_t k, ray_enc_slot_t slot, int depth,
                     const ray_enc_dict_t* dict, uint64_t* out) {
    if (depth > RAY_ENC_DEPTH_MAX) return RD_FAIL("cascade deeper than the maximum");
    if (n < 0 || n > RAY_ENC_VEC) return RD_FAIL("vector length out of range");
    if (depth == 0) return read_body(p, avail, enc, width, n, k, slot, depth, dict, out);
    if (avail < 8) return RD_FAIL("child header short");
    uint16_t rsv; memcpy(&rsv, p + 2, 2);
    uint32_t bb;  memcpy(&bb, p + 4, 4);
    if (rsv) return RD_FAIL("child header reserved bytes not zero");
    if (bb & 7) return RD_FAIL("child body size not a multiple of 8");
    if (bb > avail - 8) return RD_FAIL("child body overruns its parent");
    int64_t used = read_body(p + 8, bb, p[0], p[1], n, k, slot, depth, dict, out);
    if (used < 0) return -1;
    if (used != (int64_t)bb) return RD_FAIL("child body not fully consumed");
    return 8 + (int64_t)bb;
}


/* ---- VALUE_DICT training: distinct sorted values, at most 1<<16 ---- */
/* Bottom-up merge sort in the logical order of `k`; reentrant (no shared state). */
static void sort_kind(uint64_t* a, uint64_t* tmp, int64_t n, ray_enc_kind_t k) {
    for (int64_t w = 1; w < n; w *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * w) {
            int64_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            int64_t i = lo, j = mid, o = lo;
            while (i < mid && j < hi) tmp[o++] = ray_enc_lt(a[j], a[i], k) ? a[j++] : a[i++];
            while (i < mid) tmp[o++] = a[i++];
            while (j < hi) tmp[o++] = a[j++];
        }
        memcpy(a, tmp, sizeof(uint64_t) * (size_t)n);
    }
}

/* Returns 1 trained (*out owned by the caller, ray_free_raw), 0 no
 * dictionary (n <= 0 or more than 1<<16 distinct values), -1 out of memory. */
int ray_enc_train_dict(const uint64_t* v, int64_t n, ray_enc_kind_t k,
                       uint64_t** out, uint32_t* out_n) {
    if (n <= 0) return 0;
    uint64_t* tmp = ray_alloc_raw(sizeof(uint64_t) * 2 * (size_t)n);
    if (!tmp) return -1;
    memcpy(tmp, v, sizeof(uint64_t) * (size_t)n);
    sort_kind(tmp, tmp + n, n, k);
    int64_t d = 0;
    for (int64_t i = 0; i < n; i++) if (i == 0 || tmp[i] != tmp[d - 1]) tmp[d++] = tmp[i];
    if (d > (1 << 16)) { ray_free_raw(tmp); return 0; }
    /* ray_realloc_raw never shrinks: copy into a block sized to the distinct count. */
    uint64_t* fit = ray_alloc_raw(sizeof(uint64_t) * (size_t)d);
    if (!fit) { ray_free_raw(tmp); return -1; }
    memcpy(fit, tmp, sizeof(uint64_t) * (size_t)d);
    ray_free_raw(tmp);
    *out = fit; *out_n = (uint32_t)d;
    return 1;
}
