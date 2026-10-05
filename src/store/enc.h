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

#endif /* RAY_ENC_H */
