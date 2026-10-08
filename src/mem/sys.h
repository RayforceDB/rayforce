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
#include <stdbool.h>

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

/* Page-granular allocations off the per-thread heaps.  They count toward
 * the heap's anonymous watermark, and large ones spill to a file past it
 * (ray_heap_sys_map / ray_heap_sys_unmap, defined in mem/heap.c). */
void* ray_sys_alloc(size_t size);
void* ray_sys_realloc(void* ptr, size_t new_size);
void  ray_sys_free(void* ptr);
char* ray_sys_strdup(const char* s);
void* ray_heap_sys_map(size_t size, bool* spilled);
void  ray_heap_sys_unmap(void* p, size_t size, bool spilled);

/* --------------------------------------------------------------------------
 * Global memory accounting.  Two counters give the true runtime picture:
 *
 *   - "current" (g_sys_current)  — COMMITTED RAM: every anonymous read/write
 *     mapping (buddy pools, sys allocations, the swap-fallback pool).  This is
 *     what actually consumes physical memory.  Progress bar / budget use it.
 *   - "mapped" (g_sys_mapped)    — FILE-BACKED bytes mapped (columns, symbol
 *     file, CSV parse buffers).  Page-cache, evictable, RAM only on touch —
 *     kept separate so a read-once 100 GB column can't masquerade as live RAM.
 *
 * The low-level VM wrappers (ray_vm_alloc / _aligned / _free / _map_file /
 * _unmap_file) and the one raw-mmap site (heap swap fallback) call these so
 * every mapping is counted exactly once.  Bytes are the mapped length; add and
 * sub must balance (same length in as out).
 * -------------------------------------------------------------------------- */
void  ray_sys_track_add(int64_t bytes);       /* committed RAM +  */
void  ray_sys_track_sub(int64_t bytes);       /* committed RAM -  */
void  ray_sys_track_file_add(int64_t bytes);  /* mapped file   +  */
void  ray_sys_track_file_sub(int64_t bytes);  /* mapped file   -  */

/* Read current sys allocator counters (called by ray_mem_stats in arena.c) */
void  ray_sys_get_stat(int64_t* out_current, int64_t* out_peak);
/* Read the file-mapping counters. */
void  ray_sys_get_mapped(int64_t* out_current, int64_t* out_peak);

#endif /* RAY_MEM_SYS_H */
