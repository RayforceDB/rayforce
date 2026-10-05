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

#ifndef RAY_FSST_H
#define RAY_FSST_H

/* FSST string compression (Boncz, Neumann, Leis, VLDB 2020): a table of at
 * most 255 symbols of 1..8 bytes; code c < n expands to symbol c, code 255
 * escapes one literal byte.  Each string is compressed on its own. */

#include <stddef.h>
#include <stdint.h>

#define RAY_FSST_ESC 255

typedef struct { uint8_t n; uint8_t len[255]; uint8_t sym[255][8]; } ray_fsst_table_t;

/* Compact encode index (~0.5 KB): symbols grouped by first byte, longest first. */
typedef struct ray_fsst_index_s { uint8_t start[256]; uint8_t cnt[256]; uint8_t ids[255]; } ray_fsst_index_t;

void    ray_fsst_index_build(const ray_fsst_table_t* t, ray_fsst_index_t* ix);
/* out must hold 2*len bytes (worst case: every byte escaped). */
int64_t ray_fsst_encode_ix(const ray_fsst_table_t* t, const ray_fsst_index_t* ix,
                           const uint8_t* s, uint32_t len, uint8_t* out);

void    ray_fsst_train(const uint8_t* const* strs, const uint32_t* lens, int64_t n,
                       ray_fsst_table_t* t);
/* out must hold 2*len bytes.  Builds an index per call; use ray_fsst_encode_ix in loops. */
int64_t ray_fsst_encode(const ray_fsst_table_t* t, const uint8_t* s, uint32_t len, uint8_t* out);
int64_t ray_fsst_decode(const ray_fsst_table_t* t, const uint8_t* in, uint32_t len,
                        uint8_t* out, size_t cap);
int64_t ray_fsst_expanded_len(const ray_fsst_table_t* t, const uint8_t* in, uint32_t len);
int64_t ray_fsst_table_bytes(const ray_fsst_table_t* t);

#endif
