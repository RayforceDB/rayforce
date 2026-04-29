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

#ifndef RAY_MEM_SYS_H
#define RAY_MEM_SYS_H

#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * System-level mmap allocator for infrastructure that can't use the buddy
 * allocator (cross-thread lifetime, bootstrap, global state).
 *
 * Every allocation is tracked. ray_mem_stats() reports the totals so users
 * can see the full memory footprint.
 *
 * Each allocation prepends a 32-byte header (stores mmap size + user size),
 * so ray_sys_free() needs no size argument.
 * -------------------------------------------------------------------------- */

void* ray_sys_alloc(size_t size);
void* ray_sys_realloc(void* ptr, size_t new_size);
void  ray_sys_free(void* ptr);
char* ray_sys_strdup(const char* s);

/* Read current sys allocator counters (called by ray_mem_stats in arena.c) */
void  ray_sys_get_stat(int64_t* out_current, int64_t* out_peak);

#endif /* RAY_MEM_SYS_H */
