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
#ifndef RAY_ENCW_H
#define RAY_ENCW_H

#include <stdio.h>
#include <rayforce.h>
#include "store/enc.h"
#include "store/encfmt.h"

typedef struct {
    ray_enc_desc_t desc;
    ray_enc_buf_t  payload;
    int            n_nulls;
    bool           has_stats;
    int64_t        sum_hi;
    uint64_t       sum_lo;
    uint64_t       str_pooled;
} ray_encw_vec_t;

void      ray_encw_vec_init(ray_encw_vec_t* v);
void      ray_encw_vec_free(ray_encw_vec_t* v);
ray_err_t ray_encw_vec(int8_t type, uint8_t attrs, const void* data, const char* str_pool,
                       int64_t start, int n, const ray_enc_dict_t* dict, ray_encw_vec_t* out);
bool      ray_encw_type_has_dict(int8_t type);
bool      ray_encw_type_summable(int8_t type);
bool      ray_encw_type_supported(int8_t type);
void      ray_encw_test_force(int enc);

#endif /* RAY_ENCW_H */
