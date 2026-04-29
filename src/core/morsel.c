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

#include "core/morsel.h"
#include "core/platform.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "ops/idxop.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * ray_morsel_init
 *
 * Initialize a morsel iterator over the given vector. Sets up offset,
 * length, and element size. Issues a sequential madvise hint for mmap'd
 * vectors to optimize readahead.
 * -------------------------------------------------------------------------- */

void ray_morsel_init(ray_morsel_t* m, ray_t* vec) {
    m->vec = vec;
    m->offset = 0;
    m->len = ray_len(vec);
    m->elem_size = ray_sym_elem_size(vec->type, vec->attrs);
    m->morsel_len = 0;
    m->morsel_ptr = NULL;
    m->null_bits = NULL;

    /* One-time hint for mmap'd vectors */
    if (vec->mmod == 1) {
        ray_vm_advise_seq(ray_data(vec), (size_t)m->len * m->elem_size);
    }
}

/* --------------------------------------------------------------------------
 * ray_morsel_next
 *
 * Advance to the next morsel. Returns true if a morsel is available, false
 * when the vector is exhausted. Sets morsel_ptr to the data for the current
 * chunk, morsel_len to the number of elements, and null_bits to the null
 * bitmap (or NULL if no nulls).
 * -------------------------------------------------------------------------- */

bool ray_morsel_next(ray_morsel_t* m) {
    m->offset += m->morsel_len;
    if (m->offset >= m->len) return false;

    int64_t remaining = m->len - m->offset;
    m->morsel_len = remaining < RAY_MORSEL_ELEMS ? remaining : RAY_MORSEL_ELEMS;
    m->morsel_ptr = (uint8_t*)ray_data(m->vec) + (size_t)m->offset * m->elem_size;

    /* Null bitmap: only if HAS_NULLS.
     * M5: null_bits points to the byte containing bit (m->offset).
     * Callers must account for (m->offset % 8) bit offset within the
     * first byte of null_bits when testing individual null bits.
     *
     * HAS_INDEX path: when an accelerator index is attached, the parent's
     * 16-byte nullmap union holds the index pointer instead of bitmap data
     * (or ext_nullmap pointer).  The original bytes are preserved inside
     * ix->saved_nullmap.  Route through that snapshot here so null-aware
     * loops still see the correct bits. */
    m->null_bits = NULL;
    if (m->vec->attrs & RAY_ATTR_HAS_NULLS) {
        if (m->vec->attrs & RAY_ATTR_HAS_INDEX) {
            ray_index_t* ix = ray_index_payload(m->vec->index);
            if (ix->saved_attrs & RAY_ATTR_NULLMAP_EXT) {
                ray_t* ext;
                memcpy(&ext, &ix->saved_nullmap[0], sizeof(ext));
                m->null_bits = (uint8_t*)ray_data(ext) + (m->offset / 8);
            } else if (m->offset < 128) {
                m->null_bits = ix->saved_nullmap + (m->offset / 8);
            }
        } else if (m->vec->attrs & RAY_ATTR_NULLMAP_EXT) {
            /* External bitmap: point to correct byte offset */
            ray_t* ext = m->vec->ext_nullmap;
            m->null_bits = (uint8_t*)ray_data(ext) + (m->offset / 8);
        } else if (m->offset < 128) {
            /* Inline bitmap is 16 bytes = 128 bits; vectors with HAS_NULLS
             * and >128 elements must use external nullmap (RAY_ATTR_NULLMAP_EXT).
             * Returns null_bits=NULL for offset>=128 when using inline bitmap. */
            m->null_bits = m->vec->nullmap + (m->offset / 8);
        }
    }

    return true;
}

/* --------------------------------------------------------------------------
 * ray_morsel_init_range
 *
 * Initialize a morsel iterator over a sub-range [start, end) of the vector.
 * Used by parallel dispatch so each worker iterates a disjoint portion.
 * -------------------------------------------------------------------------- */

void ray_morsel_init_range(ray_morsel_t* m, ray_t* vec, int64_t start, int64_t end) {
    m->vec = vec;
    m->offset = start;
    m->len = end;
    m->elem_size = ray_sym_elem_size(vec->type, vec->attrs);
    m->morsel_len = 0;
    m->morsel_ptr = NULL;
    m->null_bits = NULL;
}
