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

#include "vec/vec.h"
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

    /* Null bitmap: synthesized per-morsel from sentinel reads.
     * null_bits points to a buffer offset (0,1,...) — caller indexes
     * starting at bit (m->offset & 7) just like the previous
     * source-bitmap layout did.  We mirror the (m->offset / 8) byte
     * offset by computing into &null_bits_buf[m->offset / 8].
     *
     * Synthesizing on demand sidesteps the source bitmap entirely:
     * sentinel-supporting types (F64 / F32 / integer & temporal /
     * STR / GUID) have the source bitmap stripped, so reading it
     * directly would give stale zeros.  Cost is one O(morsel_len)
     * sentinel scan per chunk; cheap given morsel_len <= 1024. */
    m->null_bits = NULL;
    if (ray_vec_may_have_nulls(m->vec)) {
        int64_t bit0 = m->offset & 7;
        int64_t base_byte = m->offset / 8;
        int64_t total_bits = bit0 + m->morsel_len;
        int64_t nbytes = (total_bits + 7) / 8;
        if ((size_t)nbytes > sizeof(m->null_bits_buf)) {
            /* Defensive — RAY_MORSEL_ELEMS bounds morsel_len to 1024
             * (=128 bytes), well within the 128-byte buffer.  Bail to
             * a NULL null_bits if a future MORSEL grows beyond. */
            return true;
        }
        memset(m->null_bits_buf, 0, (size_t)nbytes);
        for (int64_t k = 0; k < m->morsel_len; k++) {
            if (ray_vec_is_null(m->vec, m->offset + k)) {
                int64_t b = bit0 + k;
                m->null_bits_buf[b >> 3] |= (uint8_t)(1u << (b & 7));
            }
        }
        /* Mimic the prior contract: pointer addresses the byte that
         * holds bit (m->offset).  Callers index into it starting at
         * bit (m->offset & 7). */
        m->null_bits = m->null_bits_buf;
        (void)base_byte;
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
