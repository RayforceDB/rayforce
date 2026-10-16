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

/* STR codec bodies (spec "Codec bodies", STR_DICT and FSST).  Pure
 * functions over one vector; the STR_DICT entries and the FSST table are
 * side data the caller validates and passes in. */

#include "store/enc.h"
#include "store/fsst.h"
#include <rayforce.h>
#include <string.h>

static const ray_enc_kind_t K_U32 = { 32, false, false };
static inline uint64_t p8(uint64_t x) { return (x + 7) & ~(uint64_t)7; }

/* ---- STR_DICT: child `codes` ---- */
int64_t ray_enc_strdict_size(const uint64_t* codes, int n) {
    return ray_enc_choose(codes, n, K_U32, RAY_ENC_SLOT_CODES, 1, NULL).bytes;
}

bool ray_enc_strdict_write(const uint64_t* codes, int n, ray_enc_buf_t* out) {
    ray_enc_choice_t c;
    return ray_enc_write(codes, n, K_U32, RAY_ENC_SLOT_CODES, 1, NULL, out, &c);
}

int64_t ray_enc_strdict_read(const uint8_t* p, size_t avail, int n, uint32_t dict_n, uint64_t* codes) {
    int64_t a = ray_enc_read(p, avail, 0, 0, n, K_U32, RAY_ENC_SLOT_CODES, 1, NULL, codes);
    if (a < 0) return -1;                                       /* the child reader named it */
    if ((size_t)a != avail) return ray_enc_read_fail("bytes after the STR_DICT body");
    for (int i = 0; i < n; i++)
        if (codes[i] >= dict_n) return ray_enc_read_fail("STR_DICT code at or above the entry count");
    return a;
}

/* ---- FSST: child `lengths`, u64 total_bytes, compressed bytes padded to 8 ---- */
uint64_t ray_enc_fsst_encode(const ray_fsst_table_t* t, const ray_fsst_index_t* ix,
                             const char* const* s, const uint32_t* len, int n,
                             uint8_t* buf, uint64_t* clen) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t k = ray_fsst_encode_ix(t, ix, (const uint8_t*)s[i], len[i], buf + total);
        clen[i] = (uint64_t)k;
        total += (uint64_t)k;
    }
    return total;
}

int64_t ray_enc_fsst_size(const uint64_t* clen, int n, uint64_t total) {
    return ray_enc_choose(clen, n, K_U32, RAY_ENC_SLOT_FSST_LENGTHS, 1, NULL).bytes + 8 + (int64_t)p8(total);
}

bool ray_enc_fsst_write(const uint64_t* clen, int n, const uint8_t* bytes, uint64_t total,
                        ray_enc_buf_t* out) {
    if (n < 0 || n > RAY_ENC_VEC || total > SIZE_MAX - 8) return false;
    uint64_t sum = 0;                       /* total is the lengths' sum (each U32, n <= 1024) */
    for (int i = 0; i < n; i++) {
        if (clen[i] > UINT32_MAX) return false;
        sum += clen[i];
    }
    if (sum != total) return false;
    size_t at = out->len;
    ray_enc_choice_t c;
    if (ray_enc_write(clen, n, K_U32, RAY_ENC_SLOT_FSST_LENGTHS, 1, NULL, out, &c) &&
        ray_enc_buf_put(out, &total, 8) && ray_enc_buf_put(out, bytes, (size_t)total) &&
        ray_enc_buf_pad8(out))
        return true;
    out->len = at;
    return false;
}

int64_t ray_enc_fsst_check(const uint8_t* p, size_t avail, int n, const ray_fsst_table_t* t,
                           uint64_t* clen, uint64_t* xlen, const uint8_t** bytes) {
    if (n < 0 || n > RAY_ENC_VEC) return ray_enc_read_fail("FSST vector length");
    int64_t a = ray_enc_read(p, avail, 0, 0, n, K_U32, RAY_ENC_SLOT_FSST_LENGTHS, 1, NULL, clen);
    if (a < 0) return -1;                                       /* the child reader named it */
    size_t rest = avail - (size_t)a;
    if (rest < 8) return ray_enc_read_fail("FSST total short");
    uint64_t total; memcpy(&total, p + a, 8);
    rest -= 8;
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        /* The U32 kind already bounds each length; the check keeps the
         * (uint32_t) cast below exact and the sum free of overflow
         * (n <= 1024). */
        if (clen[i] > UINT32_MAX) return ray_enc_read_fail("FSST slice length above 4 GiB");
        sum += clen[i];
    }
    if (sum != total) return ray_enc_read_fail("FSST total not the sum of lengths");
    if (total > rest || p8(total) > rest) return ray_enc_read_fail("FSST compressed bytes short");
    if (p8(total) != rest) return ray_enc_read_fail("bytes after the FSST body");
    const uint8_t* b = p + a + 8;
    for (uint64_t i = total; i < p8(total); i++)
        if (b[i]) return ray_enc_read_fail("FSST padding");
    uint64_t at = 0;
    for (int i = 0; i < n; i++) {
        /* -1: a code in [n_symbols, 255) or an escape as the slice's last
         * byte (its literal would be in the next slice). */
        int64_t x = ray_fsst_expanded_len(t, b + at, (uint32_t)clen[i]);
        if (x < 0) return ray_enc_read_fail("FSST slice");
        if ((uint64_t)x > UINT32_MAX) return ray_enc_read_fail("FSST expansion above 4 GiB");
        xlen[i] = (uint64_t)x;
        at += clen[i];
    }
    *bytes = b;
    return (int64_t)avail;
}
