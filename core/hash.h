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

#ifndef HASH_H
#define HASH_H

#include "rayforce.h"
#include "ops.h"

obj_t ht_tab(u64_t size, type_t vals);
obj_t ht_set(u64_t size);
i64_t ht_tab_next(obj_t *obj, i64_t key);
i64_t ht_tab_next_with(obj_t *obj, i64_t key, hash_f hash, cmp_f cmp, nil_t *seed);
i64_t ht_tab_get(obj_t obj, i64_t key);
i64_t ht_tab_get_with(obj_t obj, i64_t key, hash_f hash, cmp_f cmp, nil_t *seed);

// Knuth's multiplicative hash
u64_t kmh_hash(i64_t key, nil_t *seed);
// FNV-1a hash
u64_t fnv1a_hash_64(i64_t key, nil_t *seed);
// Identity
u64_t i64_hash(i64_t a, nil_t *seed);

#endif
