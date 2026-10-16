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

#ifndef RAY_COW_H
#define RAY_COW_H

/*
 * cow.h -- COW (Copy-on-Write) ref counting.
 *
 * ray_retain: increment reference count
 * ray_release: decrement reference count, free when it reaches zero
 * ray_cow: copy-on-write — return same pointer if sole owner, else copy
 */

#include <rayforce.h>
#include "core/platform.h"
#include "heap.h"

/* Thread-local flag: plain (false) vs atomic (true) refcount ops.
 * Default is false (fast single-threaded path).
 * The thread pool sets true before parallel dispatch. */
extern RAY_TLS bool ray_rc_sync;

#ifdef DEBUG
/* Debug stale-pointer detector — see src/mem/heap.c (issue #240). */
void ray_dfd_check_live(const void* p, const char* who);
#endif

/* Drop one reference to v.  True when that was the last one: the caller
 * now owns the block and must free it.  ray_release is this plus ray_free;
 * the free walk in heap.c drops a dying container's children through it
 * directly, so it frees them without nesting a ray_free per level. */
RAY_INLINE bool ray_release_last(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return false;
    if (v->attrs & RAY_ATTR_ARENA) return false;
#ifdef DEBUG
    ray_dfd_check_live(v, "ray_release (stale release of freed block)");
#endif
    uint32_t prev;
    if (RAY_LIKELY(!ray_rc_sync)) {
        prev = v->rc--;
    } else {
        prev = ray_atomic_dec(&v->rc);
    }
    if (prev != 1) return false;
    if (RAY_UNLIKELY(ray_rc_sync))
        ray_atomic_fence_acquire();
    return true;
}

#endif /* RAY_COW_H */
