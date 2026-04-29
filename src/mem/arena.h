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

#ifndef RAY_ARENA_H
#define RAY_ARENA_H

#include <rayforce.h>
#include <stdbool.h>

typedef struct ray_arena ray_arena_t;

/* Create arena with given chunk size (bytes). Chunks allocated via ray_sys_alloc. */
ray_arena_t* ray_arena_new(size_t chunk_size);

/* Allocate ray_t* block with nbytes of data space.
 * Returns 32-byte aligned ray_t* with RAY_ATTR_ARENA set, rc=1.
 * Returns NULL on OOM. */
ray_t* ray_arena_alloc(ray_arena_t* arena, size_t nbytes);

/* Ensure the arena can serve subsequent allocations totalling at least
 * `bytes` without the head chunk needing to grow.  If the head chunk has
 * enough free space already, this is a no-op; otherwise a new chunk with
 * capacity >= `bytes` is allocated and becomes the head.  Returns true on
 * success, false on OOM.  Useful for making a sequence of follow-on
 * allocations infallible, which is necessary when commits to multiple
 * data structures must be atomic. */
bool ray_arena_reserve(ray_arena_t* arena, size_t bytes);

/* Total bytes currently used across every chunk in this arena.  Diagnostic
 * introspection — monotonically grows with ray_arena_alloc, resets on
 * ray_arena_reset.  Safe to call at any time. */
size_t ray_arena_total_used(const ray_arena_t* arena);

/* Reset arena — rewind all chunks to zero. Memory retained for reuse. */
void ray_arena_reset(ray_arena_t* arena);

/* Destroy arena — free all backing memory. */
void ray_arena_destroy(ray_arena_t* arena);

#endif /* RAY_ARENA_H */
