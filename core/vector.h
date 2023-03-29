/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#ifndef VECTOR_H
#define VECTOR_H

#include "rayforce.h"

extern rf_object_t list_flatten(rf_object_t object);
extern i64_t vector_i64_find(rf_object_t *vector, i64_t key);
extern i64_t vector_f64_find(rf_object_t *vector, f64_t key);
extern i64_t list_find(rf_object_t *vector, rf_object_t key);
extern i64_t vector_find(rf_object_t *vector, rf_object_t key);
extern rf_object_t vector_get(rf_object_t *vector, rf_object_t key);
extern i64_t vector_push(rf_object_t *vector, rf_object_t object);
extern i64_t vector_i64_push(rf_object_t *vector, i64_t object);
extern i64_t vector_f64_push(rf_object_t *vector, f64_t object);
extern i64_t vector_symbol_push(rf_object_t *vector, rf_object_t object);
extern i64_t list_push(rf_object_t *vector, rf_object_t object);
extern i64_t vector_i64_pop(rf_object_t *vector);
extern f64_t vector_f64_pop(rf_object_t *vector);
extern rf_object_t list_pop(rf_object_t *vector);
null_t vector_reserve(rf_object_t *vector, u32_t len);
#endif
