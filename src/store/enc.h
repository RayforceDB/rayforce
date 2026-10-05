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

#ifndef RAY_ENC_H
#define RAY_ENC_H

/* Lightweight column encodings over 1024-value vectors.  Pure codec
 * functions over arrays of T-bit patterns held in uint64_t; no file or
 * format knowledge.  The byte grammar is normative: see the spec
 * docs/superpowers/specs/2026-10-05-compressed-columns-design.md,
 * "Encodings". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAY_ENC_VEC        1024
#define RAY_ENC_CHUNK      65536
#define RAY_ENC_DEPTH_MAX  3

/* ---- bit packing (interleaved layout) ----
 * Value i of 1024 belongs to lane i % L, row i / L, L = 1024 / T.  Each
 * lane packs its T values in row order, least-significant bit first, into
 * w consecutive T-bit words; word k of lane l is stored at word index
 * k*L + l.  w == 0 stores nothing. */
size_t ray_enc_packed_bytes(int w);
void   ray_enc_pack(const uint64_t* u, int T, int w, uint8_t* out);
void   ray_enc_unpack(const uint8_t* in, int T, int w, uint64_t* u);
int    ray_enc_bitlen(uint64_t x);

/* ---- kinds and logical values ---- */
typedef struct { uint8_t T; bool is_signed; } ray_enc_kind_t;

bool    ray_enc_kind_of(int8_t type, uint8_t attrs, ray_enc_kind_t* k);
bool    ray_enc_has_bitmap(int8_t type);
int64_t ray_enc_logical(uint64_t bits, ray_enc_kind_t k);
bool    ray_enc_lt(uint64_t a, uint64_t b, ray_enc_kind_t k);

/* One vector of input: T-bit patterns, null bitmap (bit i set = row i
 * null, bitmap-carrying types only) and the null count (in-band nulls of
 * SYM are counted too, with no bitmap). */
typedef struct {
    uint64_t       bits[RAY_ENC_VEC];
    uint8_t        nullmap[RAY_ENC_VEC / 8];
    int            n;
    int            n_nulls;
    ray_enc_kind_t k;
    bool           bitmap;
} ray_enc_input_t;

bool ray_enc_load(const void* data, int8_t type, uint8_t attrs,
                  int64_t start, int n, ray_enc_input_t* in);

typedef struct { uint64_t min, max; int runs; bool all_equal; } ray_enc_stats_t;
void ray_enc_stats(const uint64_t* v, int n, ray_enc_kind_t k, ray_enc_stats_t* s);

/* ---- codec types ---- */
typedef enum {
    RAY_ENC_RAW = 0, RAY_ENC_CONST = 1, RAY_ENC_FOR_BP = 2, RAY_ENC_DELTA_BP = 3,
    RAY_ENC_RLE = 4, RAY_ENC_DICT = 5, RAY_ENC_ALP = 6, RAY_ENC_ALP_RD = 7,
    RAY_ENC_STR_DICT = 8, RAY_ENC_FSST = 9
} ray_enc_code_t;

/* Growable output buffer over the buddy heap. */
typedef struct { uint8_t* p; size_t len, cap; bool oom; } ray_enc_buf_t;
void ray_enc_buf_free(ray_enc_buf_t* b);
bool ray_enc_buf_put(ray_enc_buf_t* b, const void* src, size_t n);
bool ray_enc_buf_pad8(ray_enc_buf_t* b);

/* Body sizes in bytes (codec bodies are 8-aligned). */
int64_t ray_enc_size_raw(int n, int T);
int64_t ray_enc_size_const(void);
int     ray_enc_for_width(const uint64_t* v, int n, ray_enc_kind_t k, uint64_t* base);
int64_t ray_enc_size_for(int w);
int     ray_enc_delta_width(const uint64_t* v, int n, ray_enc_kind_t k, uint64_t* dbase);
int64_t ray_enc_size_delta(int T, int w);

#endif /* RAY_ENC_H */
