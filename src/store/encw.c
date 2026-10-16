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
#include "store/encw.h"
#include "vec/str.h"
#include "ops/idxop.h"      /* ray_i128_add */
#include "mem/heap.h"
#include "ops/hash.h"     /* ray_hash_bytes */
#include "store/col.h"       /* COL_DISK_ATTRS_MASK */
#include "core/platform.h"   /* ray_vm_map_anon */
#include <stdatomic.h>
#include <string.h>

#define ENC_F64_CANON 0x7FF8000000000000ull
#define ENC_F32_CANON 0x7FC00000ull
/* The null NaN patterns are the engine's NULL_F64 / NULL_F32 bits. */
static bool canon_matches(void) {
    double d = NULL_F64; float f = NULL_F32; uint64_t b64; uint32_t b32;
    memcpy(&b64, &d, 8); memcpy(&b32, &f, 4);
    return b64 == ENC_F64_CANON && b32 == ENC_F32_CANON;
}

bool ray_encw_type_has_dict(int8_t t) {
    return t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP ||
           t == RAY_F32 || t == RAY_F64;
}

bool ray_encw_type_summable(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 ||
           t == RAY_I64 || t == RAY_TIME;
}

bool ray_encw_type_supported(int8_t t) { return t >= RAY_BOOL && t <= RAY_STR; }

static _Atomic(int) g_encw_force = -1;
void ray_encw_test_force(int enc) { atomic_store(&g_encw_force, enc); }
static _Atomic(int) g_encw_force_float = -1;
void ray_encw_test_force_float(int enc) { atomic_store(&g_encw_force_float, enc); }
static _Atomic(int) g_encw_force_str = -1;
void ray_encw_test_force_str(int enc) { atomic_store(&g_encw_force_str, enc); }

void ray_encw_vec_init(ray_encw_vec_t* v) { memset(v, 0, sizeof *v); }
void ray_encw_vec_free(ray_encw_vec_t* v) { ray_enc_buf_free(&v->payload); }

static void vec_reset(ray_encw_vec_t* o) {
    ray_enc_buf_t keep = o->payload;
    keep.len = 0; keep.oom = false;
    memset(o, 0, sizeof *o);
    o->payload = keep;
}

static ray_err_t put(ray_encw_vec_t* o, const void* p, size_t n) {
    return ray_enc_buf_put(&o->payload, p, n) ? RAY_OK : RAY_ERR_OOM;
}
static ray_err_t pad8(ray_encw_vec_t* o) {
    return ray_enc_buf_pad8(&o->payload) ? RAY_OK : RAY_ERR_OOM;
}

/* ---- integer kinds: BOOL, U8, I16, I32, I64, DATE, TIME, TIMESTAMP, SYM ---- */
static ray_err_t encw_int(int8_t type, uint8_t attrs, const void* data, int64_t start,
                          int n, const ray_enc_dict_t* dict, ray_encw_vec_t* o) {
    ray_enc_input_t* in = ray_alloc_raw(sizeof *in);
    if (!in) return RAY_ERR_OOM;
    ray_err_t err = RAY_OK;
    if (!ray_enc_load(data, type, attrs, start, n, in)) { err = RAY_ERR_NYI; goto out; }
    o->n_nulls = in->n_nulls;
    bool nullable = type != RAY_BOOL && type != RAY_U8;
    bool all_null = nullable && in->n_nulls == n;
    if (in->bitmap && in->n_nulls > 0 && (err = put(o, in->nullmap, 128)) != RAY_OK) goto out;
    bool have = false; int64_t mn = 0, mx = 0;
    bool sum = ray_encw_type_summable(type);
    for (int i = 0; i < n; i++) {
        bool null = in->bitmap ? ((in->nullmap[i >> 3] >> (i & 7)) & 1)
                               : (type == RAY_SYM && in->bits[i] == 0);
        if (null) continue;
        int64_t x = ray_enc_logical(in->bits[i], in->k);
        if (!have || x < mn) mn = x;
        if (!have || x > mx) mx = x;
        have = true;
        if (sum) ray_i128_add(&o->sum_hi, &o->sum_lo, x);
    }
    if (all_null) {
        /* Spec "Nulls": an all-null vector is always CONST of the fill (0). */
        o->desc.encoding = RAY_ENC_CONST;
        if ((err = put(o, NULL, 8)) != RAY_OK) goto out;
    } else {
        ray_enc_choice_t c;
        int forced = atomic_load(&g_encw_force);
        bool ok = forced >= 0
            ? ray_enc_write_as((uint8_t)forced, in->bits, n, in->k, dict, &o->payload, &c)
            : ray_enc_write(in->bits, n, in->k, RAY_ENC_SLOT_TOP, 0, dict, &o->payload, &c);
        if (!ok) {
            err = o->payload.oom ? RAY_ERR_OOM : (forced >= 0 ? RAY_ERR_NYI : RAY_ERR_RANGE);
            goto out;
        }
        o->desc.encoding = c.enc;
        o->desc.width = (c.enc == RAY_ENC_FOR_BP || c.enc == RAY_ENC_DELTA_BP) ? c.width : 0;
    }
    if (have) { o->desc.min = mn; o->desc.max = mx; o->has_stats = true; }
    else      o->desc.flags |= RAY_ENC_STAT_ABSENT;
out:
    ray_free_raw(in);
    return err;
}

/* ---- F32 / F64: NaN nulls in the bitmap, bit-restore trailer ---- */
/* Float fill (spec "Nulls"): bits of rows [start, start+n), NaN rows set in
 * nullmap and filled with the minimum non-null value's bits (first of
 * ties), +0.0 when all are null.  Returns the null count; vmin / vmax get
 * the non-null extrema when has != NULL.  The extrema follow the
 * generation-0 min/max kernels' rule: strict compare, the first of tied
 * values wins (so -0.0 vs +0.0 keeps whichever came first).  No NaN is
 * left in bits, which the float dictionary order requires. */
static int float_fill(int8_t type, const void* data, int64_t start, int n, uint64_t* bits,
                      uint8_t* nullmap, double* vmin, double* vmax, bool* has) {
    const bool f32 = type == RAY_F32;
    const size_t esz = f32 ? 4 : 8;
    int nn = 0; bool have = false; double mn = 0, mx = 0; uint64_t fill = 0;
    memset(nullmap, 0, RAY_ENC_VEC / 8);
    for (int i = 0; i < n; i++) {
        uint64_t b = 0;
        memcpy(&b, (const uint8_t*)data + (size_t)(start + i) * esz, esz);
        bits[i] = b;
        double x;
        if (f32) { float f; uint32_t b32 = (uint32_t)b; memcpy(&f, &b32, 4); x = (double)f; }
        else     memcpy(&x, &b, 8);
        if (x != x) { nullmap[i >> 3] |= (uint8_t)(1u << (i & 7)); nn++; continue; }   /* every NaN is a null */
        if (!have || x < mn) { mn = x; fill = b; }
        if (!have || x > mx) mx = x;
        have = true;
    }
    if (!have) fill = 0;                    /* all null: +0.0 */
    for (int i = 0; i < n; i++) if ((nullmap[i >> 3] >> (i & 7)) & 1) bits[i] = fill;
    if (has) { *has = have; *vmin = mn; *vmax = mx; }
    return nn;
}

/* The forced-encoding test hook's ALP on a column whose first chunk trained
 * no candidate: (e, f) = (0, 0), so every value is an integer or an
 * exception.  Selection never uses it. */
static const ray_enc_alp_cands_t g_alp00 = { { { 0, 0 } }, 1 };

static ray_err_t encw_float(int8_t type, const void* data, int64_t start, int n,
                            const ray_encw_side_t* side, ray_encw_vec_t* o) {
    if (!canon_matches()) return RAY_ERR_RANGE;
    const bool f32 = type == RAY_F32;
    const size_t esz = f32 ? 4 : 8;
    const int T = f32 ? 32 : 64;
    const uint64_t canon = f32 ? ENC_F32_CANON : ENC_F64_CANON;
    const ray_enc_dict_t* dict = side ? side->dict : NULL;
    const ray_enc_alp_cands_t* alp = side ? side->alp : NULL;
    uint64_t bits[RAY_ENC_VEC];
    uint8_t  nullmap[RAY_ENC_VEC / 8];
    uint16_t tpos[RAY_ENC_VEC];
    uint64_t traw[RAY_ENC_VEC];
    int nt = 0;
    for (int i = 0; i < n; i++) {           /* trailer: non-canonical NaN payloads, before the fill */
        uint64_t b = 0;
        memcpy(&b, (const uint8_t*)data + (size_t)(start + i) * esz, esz);
        double x;
        if (f32) { float f; uint32_t b32 = (uint32_t)b; memcpy(&f, &b32, 4); x = (double)f; }
        else     memcpy(&x, &b, 8);
        if (x != x && b != canon) { tpos[nt] = (uint16_t)i; traw[nt] = b; nt++; }
    }
    double vmin = 0, vmax = 0; bool have = false;
    o->n_nulls = float_fill(type, data, start, n, bits, nullmap, &vmin, &vmax, &have);
    /* The trailer's size counts in every encoding's estimate (spec "Floats"). */
    uint64_t tb = nt ? 8 + ray_enc_pad8(2ull * (uint64_t)nt) + ray_enc_pad8((uint64_t)nt * esz) : 0;
    ray_err_t err;
    if (o->n_nulls > 0 && (err = put(o, nullmap, 128)) != RAY_OK) return err;
    ray_enc_choice_t c;
    int forced = atomic_load(&g_encw_force_float);
    if (forced < 0) forced = atomic_load(&g_encw_force);
    if (o->n_nulls == n) {
        /* Spec "Nulls": an all-null vector is always CONST of the fill (+0.0). */
        if (!ray_enc_float_write(RAY_ENC_CONST, bits, n, T, NULL, NULL, &o->payload, &c))
            return o->payload.oom ? RAY_ERR_OOM : RAY_ERR_RANGE;
    } else if (forced >= 0) {
        const ray_enc_alp_cands_t* ac = forced == RAY_ENC_ALP && (!alp || alp->n == 0) ? &g_alp00 : alp;
        if (forced > UINT8_MAX ||
            !ray_enc_float_write((uint8_t)forced, bits, n, T, dict, ac, &o->payload, &c))
            return o->payload.oom ? RAY_ERR_OOM : RAY_ERR_NYI;
    } else {
        ray_enc_choice_t want = ray_enc_float_choose(bits, n, T, dict, alp, (int64_t)tb);
        if (!ray_enc_float_write(want.enc, bits, n, T, dict, alp, &o->payload, &c))
            return o->payload.oom ? RAY_ERR_OOM : RAY_ERR_RANGE;
    }
    o->desc.encoding = c.enc;
    if (nt > 0) {
        uint16_t cnt = (uint16_t)nt;
        uint8_t zero6[6] = {0};
        if ((err = put(o, &cnt, 2)) != RAY_OK || (err = put(o, zero6, 6)) != RAY_OK) return err;
        if ((err = put(o, tpos, (size_t)nt * 2)) != RAY_OK || (err = pad8(o)) != RAY_OK) return err;
        for (int i = 0; i < nt; i++)
            if ((err = put(o, &traw[i], esz)) != RAY_OK) return err;
        if ((err = pad8(o)) != RAY_OK) return err;
        o->desc.flags |= RAY_ENC_DESC_TRAILER;
    }
    if (have) {
        uint64_t bmin, bmax;
        memcpy(&bmin, &vmin, 8); memcpy(&bmax, &vmax, 8);
        o->desc.min = (int64_t)bmin; o->desc.max = (int64_t)bmax;
        o->has_stats = true;
    } else {
        o->desc.flags |= RAY_ENC_STAT_ABSENT;
    }
    return RAY_OK;
}

/* ---- GUID: RAW or CONST; 16 zero bytes are the in-band null ---- */
static ray_err_t encw_guid(const void* data, int64_t start, int n, ray_encw_vec_t* o) {
    static const uint8_t zero[16];
    const uint8_t* p = (const uint8_t*)data + (size_t)start * 16;
    bool eq = true;
    for (int i = 0; i < n; i++) {
        if (memcmp(p + (size_t)i * 16, zero, 16) == 0) o->n_nulls++;
        if (i && memcmp(p + (size_t)i * 16, p, 16) != 0) eq = false;
    }
    uint64_t raw = (uint64_t)n * 16;
    ray_err_t err;
    int forced = atomic_load(&g_encw_force);
    if (o->n_nulls < n && forced >= 0 && forced != RAY_ENC_RAW && !(forced == RAY_ENC_CONST && eq))
        return RAY_ERR_NYI;
    if (o->n_nulls == n || (forced == RAY_ENC_CONST) || (forced < 0 && eq && 16 * 8 <= raw * 7)) {
        o->desc.encoding = RAY_ENC_CONST;
        err = put(o, o->n_nulls == n ? zero : p, 16);
    } else {
        o->desc.encoding = RAY_ENC_RAW;
        err = put(o, p, (size_t)raw);
    }
    o->desc.flags |= RAY_ENC_STAT_ABSENT;
    return err;
}

/* ---- STR: RAW (wire descriptors + vector pool), CONST, STR_DICT, FSST; "" is the null ---- */

/* The forced encoding for STR vectors (-1: selection). */
static int str_forced(void) {
    int f = atomic_load(&g_encw_force_str);
    return f >= 0 ? f : atomic_load(&g_encw_force);
}

static void sd_free(ray_encw_sdict_t* d) {
    ray_free_raw(d->slot); ray_free_raw(d->at); ray_free_raw(d->offs);
    ray_free_raw(d->lens); ray_free_raw(d->bytes);
    memset(d, 0, sizeof *d);
}

static bool sd_rehash(ray_encw_sdict_t* d, uint64_t ncap) {
    uint32_t* s = ray_calloc_raw((size_t)ncap * sizeof(uint32_t));
    if (!s) return false;
    for (uint32_t k = 0; k < d->n; k++) {
        uint64_t h = ray_hash_bytes(d->bytes + d->offs[k], d->lens[k]) & (ncap - 1);
        while (s[h]) h = (h + 1) & (ncap - 1);
        s[h] = k + 1; d->at[k] = h;
    }
    ray_free_raw(d->slot); d->slot = s; d->mask = ncap - 1;
    return true;
}

/* The entry's code, adding it when new; -1 on OOM. */
static int64_t sd_code(ray_encw_sdict_t* d, const char* p, uint32_t len) {
    if (!d->slot && !sd_rehash(d, 1024)) return -1;
    uint64_t h = ray_hash_bytes(p, len) & d->mask;
    for (;;) {
        uint32_t c1 = d->slot[h];
        if (!c1) break;
        uint32_t k = c1 - 1;
        if (d->lens[k] == len && (len == 0 || memcmp(d->bytes + d->offs[k], p, len) == 0)) return k;
        h = (h + 1) & d->mask;
    }
    if (d->n == d->cap) {
        uint32_t nc = d->cap ? d->cap * 2 : 1024;
        uint64_t* o = ray_realloc_raw(d->offs, (size_t)nc * 8);           if (o) d->offs = o;
        uint32_t* l = o ? ray_realloc_raw(d->lens, (size_t)nc * 4) : NULL; if (l) d->lens = l;
        uint64_t* a = l ? ray_realloc_raw(d->at, (size_t)nc * 8) : NULL;   if (a) d->at = a;
        if (!o || !l || !a) return -1;
        d->cap = nc;
    }
    if (!d->bytes || d->blen + len > d->bcap) {         /* never NULL: "" may be the first entry */
        uint64_t nb = d->bcap ? d->bcap : 65536;
        while (nb < d->blen + len) nb *= 2;
        char* b = ray_realloc_raw(d->bytes, (size_t)nb);
        if (!b) return -1;
        d->bytes = b; d->bcap = nb;
    }
    if (len) memcpy(d->bytes + d->blen, p, len);
    uint32_t k = d->n++;
    d->offs[k] = d->blen; d->lens[k] = len; d->blen += len;
    if (len > RAY_STR_INLINE_MAX) d->pooled += len;
    d->slot[h] = k + 1; d->at[k] = h;
    if ((uint64_t)d->n * 2 > d->mask + 1 && !sd_rehash(d, (d->mask + 1) * 2)) return -1;
    return k;
}

/* Drop the entries added since `keep`.  Removing in reverse insertion order
 * is safe for linear probing: every surviving entry was inserted before the
 * removed ones, so its probe path never crossed a removed slot; a rehash
 * re-places only entries already present, and `at` tracks every slot. */
static void sd_truncate(ray_encw_sdict_t* d, uint32_t keep) {
    while (d->n > keep) {
        uint32_t k = --d->n;
        d->slot[d->at[k]] = 0;
        d->blen = d->offs[k];
        if (d->lens[k] > RAY_STR_INLINE_MAX) d->pooled -= d->lens[k];
    }
}

typedef struct { uint8_t enc; uint64_t bytes; } str_choice_t;

/* The best plain encoding of one STR vector: RAW, CONST, FSST (smallest
 * wins, ties in that order; RAW unless the winner is at most 7/8 of it), or
 * the forced one.  An all-null vector is CONST of "" whatever is forced.
 * With fbuf != NULL the FSST codes of an FSST winner are kept for the
 * writer (*fbuf owned by the caller, ray_free_raw). */
static ray_err_t str_choose_plain(const ray_str_t* s, const char* pool, int n, const ray_encw_side_t* side,
                                  int forced, str_choice_t* out, uint8_t** fbuf, uint64_t* clen, uint64_t* ftotal) {
    uint64_t pooled = 0, sum = 0; bool eq = true; int nulls = 0;
    for (int i = 0; i < n; i++) {
        uint32_t len = s[i].len;
        nulls += len == 0; sum += len;
        if (len > RAY_STR_INLINE_MAX) pooled += len;
        if (i && eq && (len != s[0].len ||
                        memcmp(ray_str_t_ptr(&s[i], pool), ray_str_t_ptr(&s[0], pool), len) != 0)) eq = false;
    }
    uint64_t raw = (uint64_t)n * 16 + 8 + ray_enc_pad8(pooled);
    uint64_t cst = 8 + ray_enc_pad8(s[0].len);
    out->enc = RAY_ENC_RAW; out->bytes = raw;
    if (nulls == n) { out->enc = RAY_ENC_CONST; out->bytes = 8; return RAY_OK; }   /* all null: CONST of "" */
    int64_t fs = -1;
    uint8_t* fb = NULL; uint64_t cl[RAY_ENC_VEC]; uint64_t ft = 0;
    if (side && side->fsst && (forced < 0 || forced == RAY_ENC_FSST)) {
        fb = ray_alloc_raw((size_t)(2 * sum + 8));
        if (!fb) return RAY_ERR_OOM;
        const char* sp[RAY_ENC_VEC]; uint32_t sl[RAY_ENC_VEC];
        for (int i = 0; i < n; i++) { sp[i] = ray_str_t_ptr(&s[i], pool); sl[i] = s[i].len; }
        ft = ray_enc_fsst_encode(side->fsst, side->fsst_ix, sp, sl, n, fb, cl);
        bool fit = true;                                  /* the lengths child is U32 */
        for (int i = 0; i < n; i++) if (cl[i] > UINT32_MAX) fit = false;
        if (fit) fs = ray_enc_fsst_size(cl, n, ft);
        else { ray_free_raw(fb); fb = NULL; }
    }
    if (forced >= 0) {
        bool ok = forced == RAY_ENC_RAW || (forced == RAY_ENC_CONST && eq) || (forced == RAY_ENC_FSST && fs >= 0);
        if (!ok) { ray_free_raw(fb); return RAY_ERR_NYI; }
        out->enc = (uint8_t)forced;
        out->bytes = forced == RAY_ENC_CONST ? cst : forced == RAY_ENC_FSST ? (uint64_t)fs : raw;
    } else {
        if (eq && cst < out->bytes) { out->enc = RAY_ENC_CONST; out->bytes = cst; }
        if (fs >= 0 && (uint64_t)fs < out->bytes) { out->enc = RAY_ENC_FSST; out->bytes = (uint64_t)fs; }
        if (out->enc != RAY_ENC_RAW && out->bytes * 8 > raw * 7) { out->enc = RAY_ENC_RAW; out->bytes = raw; }
    }
    if (fbuf && out->enc == RAY_ENC_FSST) { *fbuf = fb; memcpy(clen, cl, (size_t)n * 8); *ftotal = ft; }
    else ray_free_raw(fb);
    return RAY_OK;
}

/* Planning pass of one STR chunk: its codes plus the dictionary entries it
 * adds, against the sum of its vectors' best plain encodings.  The chunk is
 * a STR_DICT chunk when that is at most 7/8 of the plain total (the 1/8
 * minimum gain at chunk level) or STR_DICT is forced; its entries are then
 * kept, otherwise removed.  All-null vectors are CONST either way (8 bytes
 * on both sides) and add no entry, so every entry occurs in a STR_DICT
 * vector.  Skipping them is safe only because such a column gets no
 * runtime DICT (D5 / D9): its chunk holding an all-null CONST vector has
 * no directory bounds, so ray_enccol_may_have_index never admits it, and
 * no consumer needs "" coded where it was never written.  A chunk with no non-null row is never a dictionary chunk (the
 * null image relies on it).  A chunk whose entries would pass the cap
 * abandons the accumulator. */
static ray_err_t str_plan_chunk(ray_encw_t* w, const void* data, const char* pool, int64_t n, bool* dict_chunk) {
    ray_encw_sdict_t* sd = &w->sdict;
    const ray_str_t* s = (const ray_str_t*)data;
    *dict_chunk = false;
    int forced = str_forced();
    if (forced >= 0 && forced != RAY_ENC_STR_DICT) return RAY_OK;
    bool any = false;
    for (int64_t i = 0; i < n && !any; i++) any = s[i].len != 0;
    if (!any) return RAY_OK;
    if (sd->abandoned) return forced == RAY_ENC_STR_DICT ? RAY_ERR_NYI : RAY_OK;
    uint32_t keep = sd->n; uint64_t bkeep = sd->blen;
    uint64_t codes_total = 0, plain_total = 0;
    ray_encw_side_t side = { .fsst = w->fsst, .fsst_ix = w->fsst_ix };
    uint64_t codes[RAY_ENC_VEC];
    for (int64_t at = 0; at < n; at += RAY_ENC_VEC) {
        int vn = (int)(n - at < RAY_ENC_VEC ? n - at : RAY_ENC_VEC);
        bool vany = false;
        for (int i = 0; i < vn && !vany; i++) vany = s[at + i].len != 0;
        if (!vany) { codes_total += 8; plain_total += 8; continue; }
        for (int i = 0; i < vn; i++) {
            int64_t k = sd_code(sd, ray_str_t_ptr(&s[at + i], pool), s[at + i].len);
            if (k < 0) { sd_truncate(sd, keep); return RAY_ERR_OOM; }
            codes[i] = (uint64_t)k;
        }
        codes_total += (uint64_t)ray_enc_strdict_size(codes, vn);
        if (forced < 0) {
            str_choice_t pc;
            ray_err_t err = str_choose_plain(s + at, pool, vn, &side, -1, &pc, NULL, NULL, NULL);
            if (err != RAY_OK) { sd_truncate(sd, keep); return err; }
            plain_total += pc.bytes;
        }
    }
    if (sd->n > RAY_ENCW_SDICT_CAP) {
        sd_truncate(sd, keep);
        sd->abandoned = true;
        return forced == RAY_ENC_STR_DICT ? RAY_ERR_NYI : RAY_OK;
    }
    uint64_t dict_total = codes_total + (sd->blen - bkeep) + 8ull * (sd->n - keep);
    if (forced == RAY_ENC_STR_DICT || dict_total * 8 <= plain_total * 7) { *dict_chunk = true; return RAY_OK; }
    sd_truncate(sd, keep);
    return RAY_OK;
}

/* The encoding pass of one STR vector.  side->sdict is non-NULL exactly for
 * the vectors of a STR_DICT chunk, whose entries the plan already added. */
static ray_err_t encw_str(const void* data, const char* pool, int64_t start, int n,
                          const ray_encw_side_t* side, ray_encw_vec_t* o) {
    const ray_str_t* s = (const ray_str_t*)data + start;
    uint64_t pooled = 0;
    for (int i = 0; i < n; i++) {
        if (s[i].len == 0) o->n_nulls++;
        if (s[i].len > RAY_STR_INLINE_MAX) pooled += s[i].len;
    }
    if (pooled > UINT32_MAX) return RAY_ERR_RANGE;
    o->str_occ = pooled;                                  /* the generation-0 bound */
    /* Pool order rule 2: RAW, CONST and FSST strings over 12 bytes count once per row. */
    o->str_pooled = pooled;
    o->desc.flags |= RAY_ENC_STAT_ABSENT;
    ray_err_t err;
    if (side && side->sdict && o->n_nulls < n) {          /* a STR_DICT chunk */
        ray_encw_sdict_t* sd = side->sdict;
        uint32_t planned = sd->n;
        uint64_t codes[RAY_ENC_VEC];
        int64_t mn = INT64_MAX, mx = -1;
        for (int i = 0; i < n; i++) {
            int64_t k = sd_code(sd, ray_str_t_ptr(&s[i], pool), s[i].len);
            if (k < 0) return RAY_ERR_OOM;
            codes[i] = (uint64_t)k;
            if (s[i].len == 0) continue;                  /* bounds over non-null rows (D4) */
            if (k < mn) mn = k;
            if (k > mx) mx = k;
        }
        if (sd->n != planned) return RAY_ERR_RANGE;       /* every entry comes from the plan */
        if (!ray_enc_strdict_write(codes, n, &o->payload)) return RAY_ERR_OOM;
        sd->used = true;
        o->str_pooled = 0;                                /* entries live in the dictionary (rule 1) */
        o->desc.encoding = RAY_ENC_STR_DICT;
        o->desc.flags &= (uint16_t)~RAY_ENC_STAT_ABSENT;
        o->desc.min = mn; o->desc.max = mx; o->has_stats = true;
        return RAY_OK;
    }
    str_choice_t c; uint8_t* fbuf = NULL; uint64_t clen[RAY_ENC_VEC]; uint64_t ftotal = 0;
    if ((err = str_choose_plain(s, pool, n, side, str_forced(), &c, &fbuf, clen, &ftotal)) != RAY_OK) return err;
    o->desc.encoding = c.enc;
    if (c.enc == RAY_ENC_FSST) {
        bool ok = ray_enc_fsst_write(clen, n, fbuf, ftotal, &o->payload);
        ray_free_raw(fbuf);
        if (!ok) return o->payload.oom ? RAY_ERR_OOM : RAY_ERR_RANGE;
        o->fsst_used = true;
        return RAY_OK;
    }
    if (c.enc == RAY_ENC_CONST) {
        uint32_t hdr[2] = { o->n_nulls == n ? 0 : s[0].len, 0 };
        if ((err = put(o, hdr, 8)) != RAY_OK) return err;
        if (hdr[0] && (err = put(o, ray_str_t_ptr(&s[0], pool), hdr[0])) != RAY_OK) return err;
        return pad8(o);
    }
    uint32_t off = 0;
    for (int i = 0; i < n; i++) {
        uint8_t w[16] = {0};
        uint32_t len = s[i].len;
        memcpy(w, &len, 4);
        const char* b = ray_str_t_ptr(&s[i], pool);
        if (len <= RAY_STR_INLINE_MAX) {
            memcpy(w + 4, b, len);
        } else {
            memcpy(w + 4, b, 4);
            memcpy(w + 8, &off, 4);       /* hash32 (w + 12) stays 0 */
            off += len;
        }
        if ((err = put(o, w, 16)) != RAY_OK) return err;
    }
    if ((err = put(o, &pooled, 8)) != RAY_OK) return err;
    for (int i = 0; i < n; i++)
        if (s[i].len > RAY_STR_INLINE_MAX &&
            (err = put(o, ray_str_t_ptr(&s[i], pool), s[i].len)) != RAY_OK) return err;
    return pad8(o);
}

ray_err_t ray_encw_vec_side(int8_t type, uint8_t attrs, const void* data, const char* str_pool,
                            int64_t start, int n, const ray_encw_side_t* side, ray_encw_vec_t* o) {
    if (!ray_enc_gen2_supported()) {
        return RAY_ERR_NYI;
    }
    vec_reset(o);
    if (n <= 0 || n > RAY_ENC_VEC) return RAY_ERR_RANGE;
    ray_err_t err;
    switch (type) {
    case RAY_F32: case RAY_F64: err = encw_float(type, data, start, n, side, o); break;
    case RAY_GUID:              err = encw_guid(data, start, n, o); break;
    case RAY_STR:               err = encw_str(data, str_pool, start, n, side, o); break;
    case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_SYM:
        err = encw_int(type, attrs, data, start, n,
                       ray_encw_type_has_dict(type) && side ? side->dict : NULL, o);
        break;
    default: return RAY_ERR_NYI;
    }
    if (err != RAY_OK) return err;
    if (o->n_nulls > UINT16_MAX) return RAY_ERR_RANGE;
    o->desc.n_nulls = (uint16_t)o->n_nulls;
    o->desc.payload_bytes = o->payload.len;
    return RAY_OK;
}

ray_err_t ray_encw_vec(int8_t type, uint8_t attrs, const void* data, const char* str_pool,
                       int64_t start, int n, const ray_enc_dict_t* dict, ray_encw_vec_t* out) {
    ray_encw_side_t side = { .dict = dict };
    return ray_encw_vec_side(type, attrs, data, str_pool, start, n, &side, out);
}

static ray_err_t fput(ray_encw_t* w, const void* p, size_t n) {
    static const uint8_t zeros[4096];
    while (n > 0) {
        size_t k = p ? n : (n < sizeof zeros ? n : sizeof zeros);
        if (fwrite(p ? p : zeros, 1, k, w->f) != k) return RAY_ERR_IO;
        w->pos += k; n -= k;
        if (p) break;
    }
    return RAY_OK;
}

void ray_encw_free(ray_encw_t* w) {
    ray_free_raw(w->dir); w->dir = NULL;
    ray_free_raw(w->dict_v); w->dict_v = NULL;
    sd_free(&w->sdict);
    ray_free_raw(w->fsst); w->fsst = NULL;
    ray_free_raw(w->fsst_ix); w->fsst_ix = NULL;
    ray_enc_buf_free(&w->chunk);
    ray_encw_vec_free(&w->vec);
}

ray_err_t ray_encw_begin(ray_encw_t* w, FILE* f, int8_t type, uint8_t attrs, int64_t len,
                         uint32_t sym_count, uint8_t index_kind, uint8_t col_flags) {
    memset(w, 0, sizeof *w);
    if (!ray_enc_gen2_supported()) {
        return RAY_ERR_NYI;
    }
    if (!ray_encw_type_supported(type) || len < 0) return RAY_ERR_NYI;
    w->f = f; w->type = type; w->attrs = attrs; w->len = len;
    w->sym_count = type == RAY_SYM ? sym_count : 0;
    w->index_kind = index_kind; w->col_flags = col_flags;
    w->n_chunks = ray_enc_nchunks(len);
    if (w->n_chunks > 0) {
        w->dir = ray_calloc_raw((size_t)w->n_chunks * sizeof(ray_enc_dirent_t));
        if (!w->dir) return RAY_ERR_OOM;
    }
    /* Headers and directory are written last, at offset 0 (finish). */
    return fput(w, NULL, (size_t)RAY_ENC_DIR_OFF + (size_t)w->n_chunks * sizeof(ray_enc_dirent_t));
}

void ray_encw_set_index(ray_encw_t* w, const ray_enc_idx_src_t* s) {
    w->idx = *s;
    w->has_idx = true;
    w->idx_copy = NULL; w->idx_copy_len = 0;
    w->index_kind = ray_index_payload((ray_t*)s->root)->kind;
}

void ray_encw_set_index_copy(ray_encw_t* w, uint8_t kind, const uint8_t* region, uint64_t len) {
    w->has_idx = false;
    w->idx_copy = region; w->idx_copy_len = len;
    w->index_kind = kind;
}

/* VALUE_DICT is trained on the column's first chunk and frozen (spec
 * "Encoding selection"): the filled T-bit values of every vector.  Float
 * columns go through float_fill, so no NaN reaches the dictionary, and
 * train their ALP candidates from the same values. */
static ray_err_t train_dict(ray_encw_t* w, const void* data, int64_t n) {
    ray_enc_kind_t k;
    if (!ray_enc_value_kind(w->type, w->attrs, &k)) return RAY_OK;
    uint64_t* all = ray_alloc_raw((size_t)n * sizeof(uint64_t));
    ray_enc_input_t* in = k.is_float ? NULL : ray_alloc_raw(sizeof *in);
    if (!all || (!k.is_float && !in)) { ray_free_raw(all); ray_free_raw(in); return RAY_ERR_OOM; }
    for (int64_t s = 0; s < n; s += RAY_ENC_VEC) {
        int vn = (int)(n - s < RAY_ENC_VEC ? n - s : RAY_ENC_VEC);
        if (k.is_float) {
            uint8_t nm[RAY_ENC_VEC / 8];
            (void)float_fill(w->type, data, s, vn, all + s, nm, NULL, NULL, NULL);
        } else {
            if (!ray_enc_load(data, w->type, w->attrs, s, vn, in)) {
                ray_free_raw(all); ray_free_raw(in);
                return RAY_ERR_RANGE;
            }
            memcpy(all + s, in->bits, (size_t)vn * sizeof(uint64_t));
        }
    }
    /* P0 final-review ruling: train_dict returns 1 trained, 0 no dictionary
     * (empty or more than 1<<16 distinct), -1 out of memory; OOM fails the
     * save so resident and streamed saves never diverge silently. */
    int tr = ray_enc_train_dict(all, n, k, &w->dict_v, &w->dict_n);
    if (k.is_float) ray_enc_alp_train(all, n, k.T, &w->alp);
    ray_free_raw(all); ray_free_raw(in);
    if (tr < 0) return RAY_ERR_OOM;
    if (tr == 0) { w->dict_v = NULL; w->dict_n = 0; }
    return RAY_OK;
}

/* The FSST table is trained on the column's first chunk and frozen (spec
 * "Encoding selection"); no table (no string bytes, no candidate) means no
 * FSST for the column.  Training OOM fails the save (D3), so resident and
 * streamed saves never diverge under memory pressure. */
static ray_err_t train_fsst(ray_encw_t* w, const void* data, const char* pool, int64_t n) {
    const ray_str_t* s = (const ray_str_t*)data;
    const uint8_t** sp = ray_alloc_raw((size_t)n * sizeof *sp);
    uint32_t* sl = ray_alloc_raw((size_t)n * sizeof *sl);
    w->fsst = ray_calloc_raw(sizeof *w->fsst);
    w->fsst_ix = ray_calloc_raw(sizeof *w->fsst_ix);
    if (!sp || !sl || !w->fsst || !w->fsst_ix) { ray_free_raw(sp); ray_free_raw(sl); return RAY_ERR_OOM; }
    for (int64_t i = 0; i < n; i++) { sp[i] = (const uint8_t*)ray_str_t_ptr(&s[i], pool); sl[i] = s[i].len; }
    int tr = ray_fsst_train(sp, sl, n, w->fsst);
    ray_free_raw(sp); ray_free_raw(sl);
    if (tr < 0) return RAY_ERR_OOM;
    if (tr == 0 || w->fsst->n == 0) {
        ray_free_raw(w->fsst); ray_free_raw(w->fsst_ix); w->fsst = NULL; w->fsst_ix = NULL;
        return RAY_OK;
    }
    ray_fsst_index_build(w->fsst, w->fsst_ix);
    return RAY_OK;
}

static bool type_is_float(int8_t t) { return t == RAY_F32 || t == RAY_F64; }
static double bits_double(int64_t b) { double d; memcpy(&d, &b, 8); return d; }

/* Chunk body (descriptors, then payloads) of n rows into w->chunk and its
 * directory entry into *de (offset left to the caller). */
static ray_err_t encw_chunk_body(ray_encw_t* w, const void* data, const char* str_pool, int64_t n,
                                 ray_enc_dirent_t* de) {
    ray_err_t err;
    ray_enc_dict_t dict = { w->dict_v, w->dict_n };
    w->chunk.len = 0; w->chunk.oom = false;
    if (!ray_enc_buf_put(&w->chunk, NULL, RAY_ENC_DESC_AREA)) return RAY_ERR_OOM;
    memset(de, 0, sizeof *de);
    bool have = false, fl = type_is_float(w->type);
    bool str = w->type == RAY_STR, dict_chunk = false, all_sdict = true;
    if (str && (err = str_plan_chunk(w, data, str_pool, n, &dict_chunk)) != RAY_OK) return err;
    int64_t nv = (n + RAY_ENC_VEC - 1) / RAY_ENC_VEC;
    for (int64_t j = 0; j < nv; j++) {
        int vn = (int)(n - j * RAY_ENC_VEC < RAY_ENC_VEC ? n - j * RAY_ENC_VEC : RAY_ENC_VEC);
        ray_encw_side_t side = { .dict = w->dict_n ? &dict : NULL, .alp = w->alp.n ? &w->alp : NULL,
                                 .fsst = w->fsst, .fsst_ix = w->fsst_ix,
                                 .sdict = dict_chunk ? &w->sdict : NULL };
        err = ray_encw_vec_side(w->type, w->attrs, data, str_pool, j * RAY_ENC_VEC, vn, &side, &w->vec);
        if (err != RAY_OK) return err;
        ray_enc_desc_t desc = w->vec.desc;
        desc.payload_off = w->chunk.len;
        if (!ray_enc_buf_put(&w->chunk, w->vec.payload.p, w->vec.payload.len)) return RAY_ERR_OOM;
        memcpy(w->chunk.p + j * sizeof(ray_enc_desc_t), &desc, sizeof desc);
        if (desc.encoding == RAY_ENC_DICT) w->dict_used = true;
        de->null_count += (uint32_t)w->vec.n_nulls;
        if (w->vec.n_nulls > 0 && ray_enc_has_bitmap(w->type)) w->any_bitmap_null = true;
        if (w->vec.fsst_used) w->fsst_used = true;
        if (desc.encoding != RAY_ENC_STR_DICT) all_sdict = false;
        /* Pool bound (plan Decisions, "Pool bound"): range on the per-occurrence count,
         * the generation-0 bound, though dictionary entries count once. */
        w->str_occ_bytes += w->vec.str_occ;
        if (w->str_occ_bytes > UINT32_MAX) return RAY_ERR_RANGE;
        w->str_pool_bytes += w->vec.str_pooled;
        if (ray_encw_type_summable(w->type))
            ray_i128_add128(&de->sum_hi, &de->sum_lo, w->vec.sum_hi, w->vec.sum_lo);
        if (w->vec.has_stats) {
            /* Combined in row order with the vector rule: strict compare,
             * the first of tied values wins. */
            bool lt = fl ? bits_double(desc.min) < bits_double(de->min) : desc.min < de->min;
            bool gt = fl ? bits_double(desc.max) > bits_double(de->max) : desc.max > de->max;
            if (!have || lt) de->min = desc.min;
            if (!have || gt) de->max = desc.max;
            have = true;
        }
    }
    /* STR: code bounds only when every vector is STR_DICT (D5); the combine
     * above folded only STR_DICT vectors. */
    if (!have || (str && !all_sdict)) { de->flags = RAY_ENC_STAT_ABSENT; de->min = de->max = 0; }
    de->bytes = w->chunk.len;
    return RAY_OK;
}

ray_err_t ray_encw_chunk(ray_encw_t* w, const void* data, const char* str_pool, int64_t n) {
    if (w->chunk_i >= w->n_chunks) return RAY_ERR_RANGE;
    int64_t want = w->len - w->chunk_i * RAY_ENC_CHUNK;
    if (want > RAY_ENC_CHUNK) want = RAY_ENC_CHUNK;
    if (n != want) return RAY_ERR_RANGE;
    ray_err_t err;
    if (w->chunk_i == 0 && ray_encw_type_has_dict(w->type) &&
        (err = train_dict(w, data, n)) != RAY_OK) return err;
    if (w->chunk_i == 0 && w->type == RAY_STR && (err = train_fsst(w, data, str_pool, n)) != RAY_OK) return err;
    ray_enc_dirent_t* de = &w->dir[w->chunk_i];
    if ((err = encw_chunk_body(w, data, str_pool, n, de)) != RAY_OK) return err;
    de->offset = w->pos;
    if ((err = fput(w, w->chunk.p, w->chunk.len)) != RAY_OK) return err;
    w->chunk_i++;
    return RAY_OK;
}

/* The persisted attrs of the column: the source's disk attrs, HAS_NULLS
 * for bitmap nulls. */
static uint8_t encw_final_attrs(const ray_encw_t* w, bool has_nulls) {
    return (uint8_t)((w->attrs & COL_DISK_ATTRS_MASK) | (has_nulls ? RAY_ATTR_HAS_NULLS : 0));
}

/* The 32-byte outer header and the enc header of a column of w->pos bytes. */
static void encw_headers(const ray_encw_t* w, uint64_t side_off, uint64_t side_len, bool has_nulls,
                         uint64_t index_off, ray_t* hdr, ray_enc_hdr_t* eh) {
    memset(eh, 0, sizeof *eh);
    memcpy(eh->magic, "RENC", 4);
    eh->header_version = RAY_ENC_HDR_VERSION;
    eh->index_kind = w->index_kind;
    eh->col_flags = w->col_flags;
    eh->side_off = side_off; eh->side_len = side_len;
    eh->index_off = index_off;
    eh->str_pool_bytes = w->type == RAY_STR ? w->str_pool_bytes : 0;
    eh->end_off = w->pos;
    memset(hdr, 0, sizeof *hdr);
    uint32_t xenc = RAY_ENC_XENC;
    memcpy(hdr->aux, &xenc, 4);
    hdr->order = RAY_ENC_GEN;
    hdr->type  = w->type;
    hdr->attrs = encw_final_attrs(w, has_nulls);
    hdr->rc    = w->sym_count;
    hdr->len   = w->len;
}

/* Side data (spec "Side data"): the table, then each entry 32-aligned
 * relative to the side start, in kind order; each written only when used.
 * A one-entry table is 32 bytes with its entry at 32. */
static ray_err_t encw_side(ray_encw_t* w, uint64_t* side_off, uint64_t* side_len) {
    *side_off = *side_len = 0;
    uint16_t kind[3]; uint64_t eb[3]; int ne = 0;
    size_t esz = 0;
    if (w->dict_used) {
        ray_enc_kind_t k;
        if (!ray_enc_value_kind(w->type, w->attrs, &k)) return RAY_ERR_RANGE;
        esz = (size_t)k.T / 8;                          /* F32 4 bytes, F64 8 */
        kind[ne] = RAY_ENC_SIDE_VALUE_DICT; eb[ne++] = 8 + ray_enc_pad8((uint64_t)w->dict_n * esz);
    }
    if (w->sdict.used) {
        if (w->sdict.n == 0 || w->sdict.n > RAY_ENCW_SDICT_CAP) return RAY_ERR_RANGE;
        kind[ne] = RAY_ENC_SIDE_STR_DICT; eb[ne++] = 8 + 8ull * (w->sdict.n + 1) + ray_enc_pad8(w->sdict.blen);
    }
    if (w->fsst_used) {
        if (!w->fsst || w->fsst->n == 0) return RAY_ERR_RANGE;
        kind[ne] = RAY_ENC_SIDE_FSST_TABLE; eb[ne++] = (uint64_t)ray_fsst_table_bytes(w->fsst);
    }
    if (!ne) return RAY_OK;
    ray_err_t err;
    *side_off = ray_enc_pad32(w->pos);
    if ((err = fput(w, NULL, (size_t)(*side_off - w->pos))) != RAY_OK) return err;
    uint64_t off[3], at = 8 + 24ull * (uint64_t)ne;
    for (int i = 0; i < ne; i++) { off[i] = ray_enc_pad32(at); at = off[i] + eb[i]; }
    uint8_t tbl[8 + 24 * 3]; memset(tbl, 0, sizeof tbl);
    uint32_t n32 = (uint32_t)ne; memcpy(tbl, &n32, 4);
    for (int i = 0; i < ne; i++) {
        memcpy(tbl + 8 + 24 * i, &kind[i], 2);
        memcpy(tbl + 8 + 24 * i + 8, &off[i], 8);
        memcpy(tbl + 8 + 24 * i + 16, &eb[i], 8);
    }
    if ((err = fput(w, tbl, 8 + 24 * (size_t)ne)) != RAY_OK) return err;
    uint64_t rel = 8 + 24ull * (uint64_t)ne;
    for (int i = 0; i < ne; i++) {
        if ((err = fput(w, NULL, (size_t)(off[i] - rel))) != RAY_OK) return err;
        uint64_t start = w->pos;
        if (kind[i] == RAY_ENC_SIDE_VALUE_DICT) {
            uint32_t h[2] = { w->dict_n, 0 };
            if ((err = fput(w, h, 8)) != RAY_OK) return err;
            for (uint32_t k = 0; k < w->dict_n; k++)
                if ((err = fput(w, &w->dict_v[k], esz)) != RAY_OK) return err;   /* LE low bytes */
        } else if (kind[i] == RAY_ENC_SIDE_STR_DICT) {
            uint32_t h[2] = { w->sdict.n, 0 };
            if ((err = fput(w, h, 8)) != RAY_OK) return err;
            if ((err = fput(w, w->sdict.offs, (size_t)w->sdict.n * 8)) != RAY_OK) return err;
            if ((err = fput(w, &w->sdict.blen, 8)) != RAY_OK) return err;        /* offsets[n] */
            if (w->sdict.blen && (err = fput(w, w->sdict.bytes, (size_t)w->sdict.blen)) != RAY_OK) return err;
        } else {
            const ray_fsst_table_t* t = w->fsst;
            uint8_t h[8] = { t->n, 0, 0, 0, 0, 0, 0, 0 };
            if ((err = fput(w, h, 8)) != RAY_OK) return err;
            if ((err = fput(w, t->len, t->n)) != RAY_OK) return err;
            if ((err = fput(w, NULL, (size_t)(ray_enc_pad8(t->n) - t->n))) != RAY_OK) return err;
            if ((err = fput(w, t->sym, (size_t)t->n * 8)) != RAY_OK) return err;
        }
        uint64_t used = w->pos - start;
        if (used > eb[i]) return RAY_ERR_RANGE;
        if ((err = fput(w, NULL, (size_t)(eb[i] - used))) != RAY_OK) return err;   /* pad8 */
        rel = off[i] + eb[i];
    }
    *side_len = rel;
    return RAY_OK;
}

ray_err_t ray_encw_finish(ray_encw_t* w) {
    if (w->chunk_i != w->n_chunks) return RAY_ERR_RANGE;
    ray_err_t err;
    uint64_t side_off = 0, side_len = 0;
    if ((err = encw_side(w, &side_off, &side_len)) != RAY_OK) return err;
    /* Pool order rule 1: every STR_DICT entry over 12 bytes, once. */
    if (w->sdict.used) w->str_pool_bytes += w->sdict.pooled;
    /* the index region follows the side data, 32-aligned (spec "Writers") */
    uint64_t index_off = 0;
    if (w->has_idx || w->idx_copy) {
        index_off = ray_enc_pad32(w->pos);
        if ((err = fput(w, NULL, (size_t)(index_off - w->pos))) != RAY_OK) return err;
        if (w->idx_copy) {
            if ((err = fput(w, w->idx_copy, (size_t)w->idx_copy_len)) != RAY_OK) return err;
        } else {
            ray_enc_idx_src_t s = w->idx;
            s.attrs = encw_final_attrs(w, w->any_bitmap_null);   /* SYM PART keys at the saved width */
            uint64_t nb = 0;
            ray_t* ie = ray_enc_index_write(w->f, &s, &nb);
            if (ie) { ray_err_t c = ray_err_from_obj(ie); ray_error_free(ie); return c; }
            w->pos += nb;
        }
    }
    ray_enc_hdr_t eh;
    ray_t hdr;
    encw_headers(w, side_off, side_len, w->any_bitmap_null, index_off, &hdr, &eh);
    if (fflush(w->f) != 0 || fseek(w->f, 0, SEEK_SET) != 0) return RAY_ERR_IO;
    if (fwrite(&hdr, 1, 32, w->f) != 32 || fwrite(&eh, 1, 64, w->f) != 64) return RAY_ERR_IO;
    if (w->n_chunks > 0 &&
        fwrite(w->dir, sizeof(ray_enc_dirent_t), (size_t)w->n_chunks, w->f) != (size_t)w->n_chunks)
        return RAY_ERR_IO;
    return fflush(w->f) == 0 ? RAY_OK : RAY_ERR_IO;
}

/* An all-null column image (spec "Nulls": every vector CONST of the fill,
 * statistics absent; the non-nullable BOOL / U8 read 0, with statistics).
 * Every full chunk has the same body, so two bodies are encoded (a full
 * chunk and the last) and the full one is copied (the format keeps chunks
 * contiguous, so bodies cannot be shared): the image is about n_chunks x
 * (64 B entry + one chunk body), 11.5 KiB per 65536 rows of I64 (descriptors
 * plus a 128 B bitmap and a CONST per vector; ~18 MB per 100M rows) and
 * 3.5 KiB for STR.  `fill` holds min(len, 65536) null cells of `type` in its
 * generation-0 width (SYM: `attrs` width) with `attrs` as the heap fill
 * carries them.  Headers follow ray_encw_finish exactly (persisted attrs of
 * the source, HAS_NULLS added for bitmap nulls), so the image is byte for
 * byte the file the generation-2 writer writes for the `len`-row heap fill:
 * HAS_NULLS on SYM / STR / GUID comes from the fill's attrs, as it would
 * when that vector is saved.  The image lives in a ray_vm_map_anon mapping
 * returned in *out (released by ray_vm_unmap_file, e.g. by ray_enccol_open
 * on failure or its release). */
ray_err_t ray_encw_null_image(int8_t type, uint8_t attrs, int64_t len, uint32_t sym_count,
                              const void* fill, void** out, size_t* out_size) {
    *out = NULL; *out_size = 0;
    if (!ray_enc_gen2_supported()) return RAY_ERR_NYI;
    if (!ray_encw_type_supported(type) || len < 0) return RAY_ERR_NYI;
    ray_encw_t w;
    memset(&w, 0, sizeof w);
    w.type = type; w.attrs = attrs; w.len = len;
    w.sym_count = type == RAY_SYM ? sym_count : 0;
    w.n_chunks = ray_enc_nchunks(len);
    int64_t last_rows = len - (w.n_chunks - 1) * RAY_ENC_CHUNK;
    ray_enc_buf_t full = {0};
    ray_enc_dirent_t full_de, last_de;
    memset(&full_de, 0, sizeof full_de);
    memset(&last_de, 0, sizeof last_de);
    ray_err_t err = RAY_OK;
    if (w.n_chunks > 1 || (w.n_chunks == 1 && last_rows == RAY_ENC_CHUNK)) {
        if ((err = encw_chunk_body(&w, fill, NULL, RAY_ENC_CHUNK, &full_de)) != RAY_OK) goto out;
        if (!ray_enc_buf_put(&full, w.chunk.p, w.chunk.len)) { err = RAY_ERR_OOM; goto out; }
    }
    if (w.n_chunks > 0 && last_rows < RAY_ENC_CHUNK &&
        (err = encw_chunk_body(&w, fill, NULL, last_rows, &last_de)) != RAY_OK) goto out;
    if (w.n_chunks > 0 && last_rows == RAY_ENC_CHUNK) {     /* the last chunk is full */
        last_de = full_de;
        w.chunk.len = 0;
        if (!ray_enc_buf_put(&w.chunk, full.p, full.len)) { err = RAY_ERR_OOM; goto out; }
    }
    uint64_t size = RAY_ENC_DIR_OFF + (uint64_t)w.n_chunks * sizeof(ray_enc_dirent_t);
    if (w.n_chunks > 0) size += (uint64_t)(w.n_chunks - 1) * full.len + w.chunk.len;
    uint8_t* img = ray_vm_map_anon((size_t)size);
    if (!img) { err = RAY_ERR_OOM; goto out; }
    uint64_t at = RAY_ENC_DIR_OFF + (uint64_t)w.n_chunks * sizeof(ray_enc_dirent_t);
    for (int64_t c = 0; c < w.n_chunks; c++) {
        bool last = c == w.n_chunks - 1;
        ray_enc_dirent_t de = last ? last_de : full_de;
        de.offset = at;
        memcpy(img + RAY_ENC_DIR_OFF + (size_t)c * sizeof de, &de, sizeof de);
        memcpy(img + at, last ? w.chunk.p : full.p, last ? w.chunk.len : full.len);
        at += de.bytes;
    }
    w.pos = size;
    ray_enc_hdr_t eh;
    ray_t hdr;
    encw_headers(&w, 0, 0, w.any_bitmap_null, 0, &hdr, &eh);   /* ray_encw_finish's rule */
    memcpy(img, &hdr, 32);
    memcpy(img + RAY_ENC_HDR_OFF, &eh, sizeof eh);
    *out = img; *out_size = (size_t)size;
out:
    ray_enc_buf_free(&full);
    ray_encw_free(&w);
    return err;
}
