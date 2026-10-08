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
    bool   spilled;    /* mapped over a spill file (ray_heap_sys_map) */
    /* Padding sized so the struct totals SYS_HDR_SIZE on both 32-bit
     * (WASM, size_t=4) and 64-bit (Linux/macOS, size_t=8). */
    char   _pad[SYS_HDR_SIZE - 2 * sizeof(size_t) - sizeof(bool)];
} sys_hdr_t;

_Static_assert(sizeof(sys_hdr_t) == SYS_HDR_SIZE, "sys_hdr_t must be 32 bytes");

static _Atomic(int64_t) g_sys_current    = 0;   /* committed RAM (anon RW)   */
static _Atomic(int64_t) g_sys_peak       = 0;
static _Atomic(int64_t) g_sys_mapped     = 0;   /* file-backed mapped bytes  */
static _Atomic(int64_t) g_sys_mapped_peak = 0;

/* Bump a current/peak counter pair by a signed delta (peak only on add). */
static inline void track_bump(_Atomic(int64_t)* cur_c, _Atomic(int64_t)* peak_c,
                              int64_t delta) {
    int64_t cur = atomic_fetch_add_explicit(cur_c, delta, memory_order_relaxed) + delta;
    if (delta <= 0) return;
    int64_t pk = atomic_load_explicit(peak_c, memory_order_relaxed);
    while (cur > pk) {
        if (atomic_compare_exchange_weak_explicit(peak_c, &pk, cur,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            break;
    }
}

void ray_sys_track_add(int64_t bytes)      { track_bump(&g_sys_current, &g_sys_peak,        bytes);  }
void ray_sys_track_sub(int64_t bytes)      { track_bump(&g_sys_current, &g_sys_peak,       -bytes);  }
void ray_sys_track_file_add(int64_t bytes) { track_bump(&g_sys_mapped,  &g_sys_mapped_peak,  bytes); }
void ray_sys_track_file_sub(int64_t bytes) { track_bump(&g_sys_mapped,  &g_sys_mapped_peak, -bytes); }

static inline size_t page_round(size_t n) {
    return (n + 4095) & ~(size_t)4095;
}

/* Committed-RAM accounting now lives in ray_vm_alloc / ray_vm_free (every
 * mapping funnels through them), so ray_sys_alloc/free no longer touch the
 * counter themselves — doing so would double-count. */
void* ray_sys_alloc(size_t size) {
    if (size == 0) size = 1;
    if (size > SIZE_MAX - SYS_HDR_SIZE) return NULL;
    size_t total = page_round(SYS_HDR_SIZE + size);
    bool spilled = false;
    void* p = ray_heap_sys_map(total, &spilled);
    if (!p) return NULL;

    sys_hdr_t* hdr = (sys_hdr_t*)p;
    hdr->map_size = total;
    hdr->usr_size = size;
    hdr->spilled  = spilled;
    return (char*)p + SYS_HDR_SIZE;
}

void ray_sys_free(void* ptr) {
    if (!ptr) return;
    sys_hdr_t* hdr = (sys_hdr_t*)((char*)ptr - SYS_HDR_SIZE);
    ray_heap_sys_unmap(hdr, hdr->map_size, hdr->spilled);
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
    if (out_current)
        *out_current = atomic_load_explicit(&g_sys_current, memory_order_relaxed);
    if (out_peak)
        *out_peak = atomic_load_explicit(&g_sys_peak, memory_order_relaxed);
}

void ray_sys_get_mapped(int64_t* out_current, int64_t* out_peak) {
    if (out_current)
        *out_current = atomic_load_explicit(&g_sys_mapped, memory_order_relaxed);
    if (out_peak)
        *out_peak = atomic_load_explicit(&g_sys_mapped_peak, memory_order_relaxed);
}
