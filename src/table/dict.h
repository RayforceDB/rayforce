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

#ifndef RAY_DICT_H
#define RAY_DICT_H

/*
 * dict.h -- Dict operations.
 *
 * A dict has type = RAY_DICT (99), len = 2.  Data region holds two
 * ray_t* slots:
 *   slot[0] = keys vector (any vector type)
 *   slot[1] = vals — typed vector when homogeneous, RAY_LIST otherwise
 *
 * Layout mirrors RAY_TABLE's
 * (keys, vals) shape.
 *
 * Lookup dispatches on keys->type so polymorphic keys (sym, i64, str, …)
 * all use the same probe path.  Pair count == keys->len.
 */

#include <rayforce.h>
#include "mem/heap.h"

/* Internal slot accessors — keys/vals slots in the 2-pointer block. */
static inline ray_t** ray_dict_slots(ray_t* d) {
    return (ray_t**)ray_data(d);
}

/* Lookup index of `key_atom` in `keys` vector, or -1 if not found.
 * `key_atom` may be of any atom type matching the keys vector. */
int64_t ray_dict_find_idx(ray_t* d, ray_t* key_atom);

/* Find sym key index without atom boxing.  Returns -1 if d is not a
 * RAY_DICT, keys is not RAY_SYM, or sym_id is missing. */
int64_t ray_dict_find_sym(ray_t* d, int64_t sym_id);

/* Borrowed-ref probe for sym key.  Returns the slot pointer when vals is
 * RAY_LIST; returns NULL otherwise (typed-vec dicts require boxing — use
 * ray_dict_get for those).  Used by env-path resolution where dict vals
 * are always callables/atoms/sub-dicts, never typed columns. */
ray_t* ray_dict_probe_sym_borrowed(ray_t* d, int64_t sym_id);

/* Borrowed sym-key probe for either RAY_DICT or RAY_TABLE — returns the
 * value slot for dicts and the column vector for tables.  NULL on miss. */
ray_t* ray_container_probe_sym(ray_t* v, int64_t sym_id);

#endif /* RAY_DICT_H */
