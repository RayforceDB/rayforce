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

#include "cow.h"
#include "heap.h"

/* Thread-local flag: when false (default), refcount uses plain inc/dec.
 * The thread pool sets this to true before dispatching parallel work.
 * Mirrors rayforce 1's VM->rc_sync fast path. */
RAY_TLS bool ray_rc_sync = false;

/* --------------------------------------------------------------------------
 * ray_retain
 * -------------------------------------------------------------------------- */

void ray_retain(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    if (v->attrs & RAY_ATTR_ARENA) return;
    if (RAY_LIKELY(!ray_rc_sync))
        v->rc++;
    else
        ray_atomic_inc(&v->rc);
}

/* --------------------------------------------------------------------------
 * ray_release
 * -------------------------------------------------------------------------- */

void ray_release(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return;
    if (v->attrs & RAY_ATTR_ARENA) return;
    uint32_t prev;
    if (RAY_LIKELY(!ray_rc_sync)) {
        prev = v->rc--;
    } else {
        prev = ray_atomic_dec(&v->rc);
    }
    if (prev == 1) {
        if (RAY_UNLIKELY(ray_rc_sync))
            ray_atomic_fence_acquire();
        ray_free(v);
    }
}

/* --------------------------------------------------------------------------
 * ray_cow
 * -------------------------------------------------------------------------- */

ray_t* ray_cow(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    if (v->attrs & RAY_ATTR_ARENA) return v;  /* arena-owned, no-op */
    uint32_t rc = RAY_LIKELY(!ray_rc_sync) ? v->rc : ray_atomic_load(&v->rc);
    if (rc == 1) return v;  /* sole owner -- mutate in place */
    ray_t* copy = ray_alloc_copy(v);
    if (!copy || RAY_IS_ERR(copy)) return copy;
    /* L3: ray_alloc_copy() already sets copy->rc = 1, so no redundant store needed. */
    ray_release(v);
    return copy;
}
