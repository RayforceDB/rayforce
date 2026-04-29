/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "sys.h"
#include "core/platform.h"
#include <rayforce.h>
#include <string.h>
#include <stdatomic.h>

/* 32-byte header prepended to every sys allocation.
 * mmap returns page-aligned addresses; data at page+32 is 32-byte aligned,
 * satisfying RAY_BLOCK_ALIGN for the weak ray_alloc stub. */
#define SYS_HDR_SIZE 32

typedef struct {
    size_t map_size;   /* total mmap'd bytes (header + user, page-rounded) */
    size_t usr_size;   /* user-requested bytes (for realloc memcpy) */
    char   _pad[16];
} sys_hdr_t;

_Static_assert(sizeof(sys_hdr_t) == SYS_HDR_SIZE, "sys_hdr_t must be 32 bytes");

static _Atomic(int64_t) g_sys_current = 0;
static _Atomic(int64_t) g_sys_peak    = 0;

static inline size_t page_round(size_t n) {
    return (n + 4095) & ~(size_t)4095;
}

void* ray_sys_alloc(size_t size) {
    if (size == 0) size = 1;
    if (size > SIZE_MAX - SYS_HDR_SIZE) return NULL;
    size_t total = page_round(SYS_HDR_SIZE + size);
    void* p = ray_vm_alloc(total);
    if (!p) return NULL;

    sys_hdr_t* hdr = (sys_hdr_t*)p;
    hdr->map_size = total;
    hdr->usr_size = size;

    int64_t cur = atomic_fetch_add_explicit(&g_sys_current, (int64_t)total,
                                             memory_order_relaxed) + (int64_t)total;
    int64_t pk = atomic_load_explicit(&g_sys_peak, memory_order_relaxed);
    while (cur > pk) {
        if (atomic_compare_exchange_weak_explicit(&g_sys_peak, &pk, cur,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed))
            break;
    }

    return (char*)p + SYS_HDR_SIZE;
}

void ray_sys_free(void* ptr) {
    if (!ptr) return;
    sys_hdr_t* hdr = (sys_hdr_t*)((char*)ptr - SYS_HDR_SIZE);
    size_t total = hdr->map_size;
    ray_vm_free(hdr, total);
    atomic_fetch_sub_explicit(&g_sys_current, (int64_t)total,
                               memory_order_relaxed);
}

/* L5: ray_sys_realloc(ptr, 0) frees ptr and returns NULL, matching the
 * behavior of some realloc implementations. Callers should not rely on
 * this as a general-purpose free — use ray_sys_free() explicitly. */
void* ray_sys_realloc(void* ptr, size_t new_size) {
    if (!ptr) return ray_sys_alloc(new_size);
    if (new_size == 0) { ray_sys_free(ptr); return NULL; }
    if (new_size > SIZE_MAX - SYS_HDR_SIZE) return NULL;

    sys_hdr_t* old_hdr = (sys_hdr_t*)((char*)ptr - SYS_HDR_SIZE);
    size_t old_usr = old_hdr->usr_size;
    size_t new_total = page_round(SYS_HDR_SIZE + new_size);

    /* Same page count — just update user size */
    if (new_total == old_hdr->map_size) {
        old_hdr->usr_size = new_size;
        return ptr;
    }

    void* new_ptr = ray_sys_alloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_usr < new_size ? old_usr : new_size);
    ray_sys_free(ptr);
    return new_ptr;
}

char* ray_sys_strdup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* dup = (char*)ray_sys_alloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

void ray_sys_get_stat(int64_t* out_current, int64_t* out_peak) {
    *out_current = atomic_load_explicit(&g_sys_current, memory_order_relaxed);
    *out_peak    = atomic_load_explicit(&g_sys_peak, memory_order_relaxed);
}
