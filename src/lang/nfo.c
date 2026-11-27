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
    /* A part that fails to allocate: no nfo (NULL), the parts made freed.
     * An error object here would be kept by callers, which take the nfo as
     * optional and release it (a no-op on an error), and read as a list. */
    ray_t* fname = ray_str(filename, fname_len);
    ray_t* src   = fname && !RAY_IS_ERR(fname) ? ray_str(source, src_len) : NULL;
    ray_t* keys  = src && !RAY_IS_ERR(src) ? ray_vec_new(RAY_I64, 0) : NULL;
    ray_t* vals  = keys && !RAY_IS_ERR(keys) ? ray_vec_new(RAY_I64, 0) : NULL;
    ray_t* nfo   = vals && !RAY_IS_ERR(vals) ? ray_alloc(4 * sizeof(ray_t*)) : NULL;
    if (!nfo || RAY_IS_ERR(nfo)) {
        ray_t* parts[5] = { fname, src, keys, vals, nfo };
        for (int i = 0; i < 5; i++) {
            if (parts[i] && RAY_IS_ERR(parts[i])) ray_error_free(parts[i]);
            else if (parts[i]) ray_release(parts[i]);
        }
        return NULL;
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

    /* If vec_append reallocated, update the nfo list slots directly.
     * ray_vec_append already transferred ownership (the old pointer is
     * invalid), so we just swap the slot pointer; no retain/release needed
     * since the list already owns one ref from the original append.  An
     * append that fails to grow leaves its vector as it was: the span goes
     * unrecorded, its key taken back if only the value failed, and the
     * error is freed, never kept in a slot. */
    ray_t** slots = (ray_t**)ray_data(nfo);
    ray_t* new_keys = ray_vec_append(keys, &key);
    if (!new_keys || RAY_IS_ERR(new_keys)) { ray_error_free(new_keys); return; }
    if (new_keys != keys) slots[2] = new_keys;
    ray_t* new_vals = ray_vec_append(vals, &val);
    if (!new_vals || RAY_IS_ERR(new_vals)) { ray_error_free(new_vals); new_keys->len--; return; }
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

    /* Newest first: the parser frees some nodes it recorded (a lambda's
     * parameter vector, for one), and a node allocated later at the same
     * address is recorded after them.  Only the newest record can name
     * the live node; the first match named the freed one's span. */
    for (int64_t i = n - 1; i >= 0; i--) {
        if (kdata[i] == needle) {
            ray_span_t span;
            span.id = vdata[i];
            return span;
        }
    }

    return none;
}
