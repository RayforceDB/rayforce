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
#include "table/sym.h"
#include <string.h>

static inline uint64_t tmask(int T) { return T == 64 ? ~0ull : ((1ull << T) - 1); }

bool ray_enc_kind_of(int8_t type, uint8_t attrs, ray_enc_kind_t* k) {
    switch (type) {
    case RAY_BOOL: case RAY_U8:       *k = (ray_enc_kind_t){ 8,  false, false }; return true;
    case RAY_I16:                     *k = (ray_enc_kind_t){ 16, true,  false }; return true;
    case RAY_I32: case RAY_DATE:
    case RAY_TIME:                    *k = (ray_enc_kind_t){ 32, true,  false }; return true;
    case RAY_I64: case RAY_TIMESPAN: case RAY_TIMESTAMP: *k = (ray_enc_kind_t){ 64, true,  false }; return true;
    case RAY_SYM:
        *k = (ray_enc_kind_t){ (uint8_t)(8 * RAY_SYM_ELEM(attrs)), false, false };
        return true;
    default: return false;
    }
}

bool ray_enc_value_kind(int8_t type, uint8_t attrs, ray_enc_kind_t* k) {
    if (type == RAY_F32) { *k = (ray_enc_kind_t){ 32, false, true }; return true; }
    if (type == RAY_F64) { *k = (ray_enc_kind_t){ 64, false, true }; return true; }
    return ray_enc_kind_of(type, attrs, k);
}

bool ray_enc_has_bitmap(int8_t type) {
    switch (type) {
    case RAY_I16: case RAY_I32: case RAY_I64: case RAY_DATE: case RAY_TIME:
    case RAY_TIMESPAN: case RAY_TIMESTAMP: case RAY_F32: case RAY_F64: return true;
    default: return false;
    }
}

bool ray_enc_load(const void* data, int8_t type, uint8_t attrs,
                  int64_t start, int n, ray_enc_input_t* in) {
    if (n < 0 || n > RAY_ENC_VEC) return false;
    if (!ray_enc_kind_of(type, attrs, &in->k)) return false;
    in->n = n;
    in->n_nulls = 0;
    in->bitmap = ray_enc_has_bitmap(type);
    memset(in->nullmap, 0, sizeof in->nullmap);
    const uint64_t m = tmask(in->k.T);
    for (int i = 0; i < n; i++) {
        int64_t r = start + i;
        uint64_t b; bool null = false;
        switch (type) {
        case RAY_BOOL: case RAY_U8: b = ((const uint8_t*)data)[r]; break;
        case RAY_I16: { int16_t v = ((const int16_t*)data)[r]; null = v == NULL_I16; b = (uint64_t)(uint16_t)v; break; }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            { int32_t v = ((const int32_t*)data)[r]; null = v == NULL_I32; b = (uint64_t)(uint32_t)v; break; }
        case RAY_I64: case RAY_TIMESPAN: case RAY_TIMESTAMP:
            { int64_t v = ((const int64_t*)data)[r]; null = v == NULL_I64; b = (uint64_t)v; break; }
        case RAY_SYM: { int64_t v = ray_read_sym(data, r, type, attrs); null = v == 0; b = (uint64_t)v; break; }
        default: return false;
        }
        in->bits[i] = b & m;
        if (null) {
            in->n_nulls++;
            if (in->bitmap) in->nullmap[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
    }
    if (in->bitmap && in->n_nulls > 0) {
        bool have = false; uint64_t mn = 0;
        for (int i = 0; i < n; i++) {
            if ((in->nullmap[i >> 3] >> (i & 7)) & 1) continue;
            if (!have || ray_enc_lt(in->bits[i], mn, in->k)) { mn = in->bits[i]; have = true; }
        }
        if (!have) mn = 0;                          /* all null: fill zero */
        for (int i = 0; i < n; i++)
            if ((in->nullmap[i >> 3] >> (i & 7)) & 1) in->bits[i] = mn;
    }
    return true;
}

/* The extrema are taken on ray_enc_okey keys, which map the T-bit patterns
 * one to one, so a key names its pattern back. */
void ray_enc_stats(const uint64_t* v, int n, ray_enc_kind_t k, ray_enc_stats_t* s) {
    if (n <= 0) { s->min = s->max = 0; s->runs = 0; s->all_equal = true; return; }
    const uint64_t m = tmask(k.T), sb = 1ull << (k.T - 1);
    uint64_t kmn = ray_enc_okey(v[0], k), kmx = kmn;
    int runs = 1;
    if (k.is_float) {
        for (int i = 1; i < n; i++) {
            uint64_t x = ray_enc_okey(v[i], k);
            kmn = x < kmn ? x : kmn; kmx = x > kmx ? x : kmx;
            runs += v[i] != v[i - 1];
        }
        s->min = kmn ^ ((kmn & sb) ? sb : m);
        s->max = kmx ^ ((kmx & sb) ? sb : m);
    } else {
        const uint64_t flip = k.is_signed ? sb : 0;
        for (int i = 1; i < n; i++) {
            uint64_t x = (v[i] & m) ^ flip;
            kmn = x < kmn ? x : kmn; kmx = x > kmx ? x : kmx;
            runs += v[i] != v[i - 1];
        }
        s->min = kmn ^ flip; s->max = kmx ^ flip;
    }
    s->runs = runs;
    s->all_equal = runs <= 1;
}
