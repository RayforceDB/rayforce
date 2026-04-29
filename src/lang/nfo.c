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

#include "nfo.h"
#include <stdint.h>

ray_t* ray_nfo_create(const char* filename, size_t fname_len,
                      const char* source,   size_t src_len) {
    ray_t* fname = ray_str(filename, fname_len);
    if (RAY_IS_ERR(fname)) return fname;

    ray_t* src = ray_str(source, src_len);
    if (RAY_IS_ERR(src)) { ray_release(fname); return src; }

    ray_t* keys = ray_vec_new(RAY_I64, 0);
    if (RAY_IS_ERR(keys)) { ray_release(fname); ray_release(src); return keys; }

    ray_t* vals = ray_vec_new(RAY_I64, 0);
    if (RAY_IS_ERR(vals)) { ray_release(fname); ray_release(src); ray_release(keys); return vals; }

    /* Build the nfo list: alloc 4-slot list, set elements directly. */
    ray_t* nfo = ray_alloc(4 * sizeof(ray_t*));
    if (!nfo || RAY_IS_ERR(nfo)) {
        ray_release(fname); ray_release(src); ray_release(keys); ray_release(vals);
        return ray_error("oom", NULL);
    }
    nfo->type = RAY_LIST;
    nfo->len = 4;
    ray_t** elems = (ray_t**)ray_data(nfo);
    elems[0] = fname;   /* ownership transfers — no extra retain needed */
    elems[1] = src;
    elems[2] = keys;
    elems[3] = vals;

    return nfo;
}

void ray_nfo_insert(ray_t* nfo, ray_t* node, ray_span_t span) {
    int64_t key = (intptr_t)node;
    int64_t val = span.id;

    ray_t* keys = NFO_KEYS(nfo);
    ray_t* vals = NFO_VALS(nfo);
    if (!keys || !vals) return;

    ray_t* new_keys = ray_vec_append(keys, &key);
    ray_t* new_vals = ray_vec_append(vals, &val);

    /* If vec_append reallocated, update the nfo list slots directly.
     * ray_vec_append already transferred ownership (the old pointer is
     * invalid), so we just swap the slot pointer; no retain/release needed
     * since the list already owns one ref from the original append. */
    ray_t** slots = (ray_t**)ray_data(nfo);
    if (new_keys != keys) slots[2] = new_keys;
    if (new_vals != vals) slots[3] = new_vals;
}

ray_span_t ray_nfo_get(ray_t* nfo, ray_t* node) {
    ray_span_t none = { .id = 0 };
    if (!nfo) return none;

    ray_t* keys = NFO_KEYS(nfo);
    ray_t* vals = NFO_VALS(nfo);
    if (!keys || !vals) return none;

    int64_t needle = (intptr_t)node;
    int64_t n      = ray_len(keys);
    int64_t* kdata = (int64_t*)ray_data(keys);
    int64_t* vdata = (int64_t*)ray_data(vals);

    for (int64_t i = 0; i < n; i++) {
        if (kdata[i] == needle) {
            ray_span_t span;
            span.id = vdata[i];
            return span;
        }
    }

    return none;
}
