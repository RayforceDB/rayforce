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
#include "store/col.h"
#include "table/sym.h"
#include "vec/str.h"
#include "ops/idxop.h"      /* ray_i128_add */
#include <stdatomic.h>
#include <string.h>

#define ENC_F64_CANON 0x7FF8000000000000ull
#define ENC_F32_CANON 0x7FC00000ull

bool ray_encw_type_has_dict(int8_t t) {
    return t == RAY_I16 || t == RAY_I32 || t == RAY_I64 ||
           t == RAY_DATE || t == RAY_TIME || t == RAY_TIMESTAMP;
}

bool ray_encw_type_summable(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 ||
           t == RAY_I64 || t == RAY_TIME;
}

bool ray_encw_type_supported(int8_t t) { return t >= RAY_BOOL && t <= RAY_STR; }

static _Atomic(int) g_encw_force = -1;
void ray_encw_test_force(int enc) { atomic_store(&g_encw_force, enc); }

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

/* ---- F32 / F64: RAW or CONST, NaN nulls in the bitmap, bit-restore trailer ---- */
static ray_err_t encw_float(int8_t type, const void* data, int64_t start, int n,
                            ray_encw_vec_t* o) {
    const bool f32 = type == RAY_F32;
    const size_t esz = f32 ? 4 : 8;
    const uint64_t canon = f32 ? ENC_F32_CANON : ENC_F64_CANON;
    uint64_t bits[RAY_ENC_VEC];
    uint8_t  nullmap[RAY_ENC_VEC / 8];
    uint16_t tpos[RAY_ENC_VEC];
    uint64_t traw[RAY_ENC_VEC];
    int nt = 0;
    memset(nullmap, 0, sizeof nullmap);
    double vmin = 0, vmax = 0; bool have = false; uint64_t fill = 0;
    for (int i = 0; i < n; i++) {
        uint64_t b = 0;
        memcpy(&b, (const uint8_t*)data + (size_t)(start + i) * esz, esz);
        bits[i] = b;
        double x;
        if (f32) { float f; uint32_t b32 = (uint32_t)b; memcpy(&f, &b32, 4); x = (double)f; }
        else     memcpy(&x, &b, 8);
        if (x != x) {                       /* every NaN is a null */
            nullmap[i >> 3] |= (uint8_t)(1u << (i & 7));
            o->n_nulls++;
            if (b != canon) { tpos[nt] = (uint16_t)i; traw[nt] = b; nt++; }
            continue;
        }
        /* The generation-0 min/max kernels' rule: strict compare, the first
         * of tied values wins (so -0.0 vs +0.0 keeps whichever came first). */
        if (!have || x < vmin) { vmin = x; fill = b; }
        if (!have || x > vmax) vmax = x;
        have = true;
    }
    if (!have) fill = 0;                    /* all null: +0.0 */
    for (int i = 0; i < n; i++)
        if ((nullmap[i >> 3] >> (i & 7)) & 1) bits[i] = fill;
    bool eq = true;
    for (int i = 1; i < n && eq; i++) if (bits[i] != bits[0]) eq = false;
    ray_err_t err;
    if (o->n_nulls > 0 && (err = put(o, nullmap, 128)) != RAY_OK) return err;
    uint64_t raw_bytes = ray_enc_pad8((uint64_t)n * esz);
    /* The trailer's size counts in every encoding's estimate (spec "Floats"). */
    uint64_t tb = nt ? 8 + ray_enc_pad8(2ull * (uint64_t)nt) + ray_enc_pad8((uint64_t)nt * esz) : 0;
    int forced = atomic_load(&g_encw_force);
    bool use_const = o->n_nulls == n ||
        (forced == RAY_ENC_CONST ? eq
         : forced == RAY_ENC_RAW ? false
         : (eq && (8 + tb) * 8 <= (raw_bytes + tb) * 7));
    if (forced >= 0 && forced != RAY_ENC_RAW && forced != RAY_ENC_CONST) return RAY_ERR_NYI;
    if (forced == RAY_ENC_CONST && !use_const) return RAY_ERR_NYI;
    if (use_const) {
        o->desc.encoding = RAY_ENC_CONST;
        if ((err = put(o, &bits[0], esz)) != RAY_OK || (err = pad8(o)) != RAY_OK) return err;
    } else {
        o->desc.encoding = RAY_ENC_RAW;
        for (int i = 0; i < n; i++)
            if ((err = put(o, &bits[i], esz)) != RAY_OK) return err;
        if ((err = pad8(o)) != RAY_OK) return err;
    }
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
    if (forced >= 0 && forced != RAY_ENC_RAW && !(forced == RAY_ENC_CONST && eq)) return RAY_ERR_NYI;
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

/* ---- STR: RAW (wire descriptors + vector pool) or CONST; "" is the null ---- */
static ray_err_t encw_str(const void* data, const char* pool, int64_t start, int n,
                          ray_encw_vec_t* o) {
    const ray_str_t* s = (const ray_str_t*)data + start;
    uint64_t pooled = 0; bool eq = true;
    for (int i = 0; i < n; i++) {
        uint32_t len = s[i].len;
        if (len == 0) o->n_nulls++;
        if (len > RAY_STR_INLINE_MAX) pooled += len;
        if (i && eq && (len != s[0].len ||
                        memcmp(ray_str_t_ptr(&s[i], pool), ray_str_t_ptr(&s[0], pool), len) != 0))
            eq = false;
    }
    if (pooled > UINT32_MAX) return RAY_ERR_RANGE;
    /* Pool order rule 2: RAW and CONST strings over 12 bytes count once per row. */
    o->str_pooled = pooled;
    uint64_t raw = (uint64_t)n * 16 + 8 + ray_enc_pad8(pooled);
    uint64_t cst = 8 + ray_enc_pad8(s[0].len);
    ray_err_t err;
    o->desc.flags |= RAY_ENC_STAT_ABSENT;
    int forced = atomic_load(&g_encw_force);
    if (forced >= 0 && forced != RAY_ENC_RAW && !(forced == RAY_ENC_CONST && eq)) return RAY_ERR_NYI;
    if (o->n_nulls == n || forced == RAY_ENC_CONST || (forced < 0 && eq && cst * 8 <= raw * 7)) {
        o->desc.encoding = RAY_ENC_CONST;
        uint32_t hdr[2] = { o->n_nulls == n ? 0 : s[0].len, 0 };
        if ((err = put(o, hdr, 8)) != RAY_OK) return err;
        if (hdr[0] && (err = put(o, ray_str_t_ptr(&s[0], pool), hdr[0])) != RAY_OK) return err;
        return pad8(o);
    }
    o->desc.encoding = RAY_ENC_RAW;
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

ray_err_t ray_encw_vec(int8_t type, uint8_t attrs, const void* data, const char* str_pool,
                       int64_t start, int n, const ray_enc_dict_t* dict, ray_encw_vec_t* o) {
#if !RAY_ENC_LE
    (void)type; (void)attrs; (void)data; (void)str_pool; (void)start; (void)n; (void)dict; (void)o;
    return RAY_ERR_NYI;
#else
    vec_reset(o);
    if (n <= 0 || n > RAY_ENC_VEC) return RAY_ERR_RANGE;
    ray_err_t err;
    switch (type) {
    case RAY_F32: case RAY_F64: err = encw_float(type, data, start, n, o); break;
    case RAY_GUID:              err = encw_guid(data, start, n, o); break;
    case RAY_STR:               err = encw_str(data, str_pool, start, n, o); break;
    case RAY_BOOL: case RAY_U8: case RAY_I16: case RAY_I32: case RAY_I64:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_SYM:
        err = encw_int(type, attrs, data, start, n,
                       ray_encw_type_has_dict(type) ? dict : NULL, o);
        break;
    default: return RAY_ERR_NYI;
    }
    if (err != RAY_OK) return err;
    if (o->n_nulls > UINT16_MAX) return RAY_ERR_RANGE;
    o->desc.n_nulls = (uint16_t)o->n_nulls;
    o->desc.payload_bytes = o->payload.len;
    return RAY_OK;
#endif
}
